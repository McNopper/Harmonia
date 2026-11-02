// GPU tests of the production Slang solver. Oracle: analytic virtual-light geometry.
#include <volk/volk.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <numbers>
#include <slang-math/slang-math.hpp>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "fixtures/VulkanTestFixture.hpp"
#include "harmonia/core/Buffer.hpp"
#include "harmonia/core/ShaderModule.hpp"
#include "harmonia/core/VulkanHandle.hpp"
#include "harmonia/rt/TlasBuilder.hpp"
#include "harmonia/scene_rt/TriangleMesh.hpp"

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
    template <typename Input, typename Output>
    void runCompute(std::span<const Input> inputs,
                    std::vector<Output>& results,
                    const char* shader,
                    VkAccelerationStructureKHR tlas = VK_NULL_HANDLE) {
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
        bufferInfo.size = inputs.size() * sizeof(Output);
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
        results.resize(inputs.size());
        std::memset(mapped.pMappedData, 0xCD, results.size() * sizeof(Output));
        ASSERT_EQ(vmaFlushAllocation(ctx.allocator, target.allocation, 0, VK_WHOLE_SIZE), VK_SUCCESS);

        const VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(SmsPush)};
        harmonia::core::UniqueDescriptorSetLayout setLayout;
        if (tlas != VK_NULL_HANDLE) {
            const VkDescriptorSetLayoutBinding binding{
                0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
            const VkDescriptorSetLayoutCreateInfo info{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                       .pNext = nullptr,
                                                       .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT,
                                                       .bindingCount = 1,
                                                       .pBindings = &binding};
            VkDescriptorSetLayout raw{};
            ASSERT_EQ(vkCreateDescriptorSetLayout(ctx.device, &info, nullptr, &raw), VK_SUCCESS);
            setLayout = harmonia::core::UniqueDescriptorSetLayout{ctx.device, raw};
        }
        const VkPipelineLayoutCreateInfo layoutInfo{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                                    .pNext = nullptr,
                                                    .flags = 0,
                                                    .setLayoutCount = tlas == VK_NULL_HANDLE ? 0U : 1U,
                                                    .pSetLayouts = tlas == VK_NULL_HANDLE ? nullptr : setLayout.ptr(),
                                                    .pushConstantRangeCount = 1,
                                                    .pPushConstantRanges = &range};
        VkPipelineLayout rawLayout{};
        ASSERT_EQ(vkCreatePipelineLayout(ctx.device, &layoutInfo, nullptr, &rawLayout), VK_SUCCESS);
        const harmonia::core::UniquePipelineLayout layout{ctx.device, rawLayout};
        auto rawModule =
            harmonia::core::createShaderModule(ctx.device, std::filesystem::path(HARMONIA_TEST_SHADER_DIR) / shader);
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
        if (tlas != VK_NULL_HANDLE) {
            const VkMemoryBarrier2 accelerationReady{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                                     .pNext = nullptr,
                                                     .srcStageMask =
                                                         VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                                                     .srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                                                     .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                                     .dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR};
            const VkDependencyInfo accelerationDependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                                          .pNext = nullptr,
                                                          .dependencyFlags = 0,
                                                          .memoryBarrierCount = 1,
                                                          .pMemoryBarriers = &accelerationReady,
                                                          .bufferMemoryBarrierCount = 0,
                                                          .pBufferMemoryBarriers = nullptr,
                                                          .imageMemoryBarrierCount = 0,
                                                          .pImageMemoryBarriers = nullptr};
            vkCmdPipelineBarrier2(*cmd, &accelerationDependency);
            const VkWriteDescriptorSetAccelerationStructureKHR acceleration{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
                .pNext = nullptr,
                .accelerationStructureCount = 1,
                .pAccelerationStructures = &tlas};
            const VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                             .pNext = &acceleration,
                                             .dstSet = VK_NULL_HANDLE,
                                             .dstBinding = 0,
                                             .dstArrayElement = 0,
                                             .descriptorCount = 1,
                                             .descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
                                             .pImageInfo = nullptr,
                                             .pBufferInfo = nullptr,
                                             .pTexelBufferView = nullptr};
            const VkPushDescriptorSetInfo info{.sType = VK_STRUCTURE_TYPE_PUSH_DESCRIPTOR_SET_INFO,
                                               .pNext = nullptr,
                                               .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
                                               .layout = layout,
                                               .set = 0,
                                               .descriptorWriteCount = 1,
                                               .pDescriptorWrites = &write};
            vkCmdPushDescriptorSet2(*cmd, &info);
        }
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
        const auto* output = static_cast<const Output*>(mapped.pMappedData);
        results.assign(output, output + inputs.size());
    }

    void run(std::span<const SmsInput> inputs, std::vector<SmsOutput>& results) {
        runCompute<SmsInput, SmsOutput>(inputs, results, "sms_test.spv");
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

TEST_F(SmsFixture, SmsReciprocalCountsThroughSuccessWithoutTrialCap) {
    using Words = std::array<std::uint32_t, 4>;
    const std::array inputs{Words{0, 1, 0, 0},
                            Words{127, 16, 0, 0},
                            Words{257, 4, 0, 0},
                            Words{712, 1, 0, 0},
                            Words{712, 7, 0, 0},
                            Words{712, 128, 0, 0}};
    std::vector<Words> outputs;
    ASSERT_NO_FATAL_FAILURE((runCompute<Words, Words>(inputs, outputs, "sms_reciprocal_test.spv")));
    ASSERT_EQ(outputs.size(), inputs.size());
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        SCOPED_TRACE(i);
        EXPECT_EQ(outputs[i][0], inputs[i][0] + 1U);
        EXPECT_EQ(outputs[i][1], 0U);
        EXPECT_EQ(outputs[i][2], 1U);
        EXPECT_EQ(outputs[i][3], std::bit_cast<std::uint32_t>(static_cast<float>(inputs[i][0] + 1U)))
            << "Wrong weight or an unfinished reciprocal estimate was published";
    }
}

TEST_F(SmsFixture, SmsReciprocalCarryAndOverflowAreExplicit) {
    using Words = std::array<std::uint32_t, 4>;
    const std::array inputs{Words{1, 1, 0xFFFFFFFEU, 0}, Words{0, 1, 0xFFFFFFFFU, 0xFFFFFFFFU}};
    std::vector<Words> outputs;
    ASSERT_NO_FATAL_FAILURE((runCompute<Words, Words>(inputs, outputs, "sms_reciprocal_test.spv")));
    ASSERT_EQ(outputs.size(), inputs.size());
    EXPECT_EQ(outputs[0], (Words{0, 1, 1, std::bit_cast<std::uint32_t>(4294967296.0F)}));
    EXPECT_EQ(outputs[1], (Words{0, 0, 2, 0}));
}

namespace {
struct SmsTrialInput {
    std::array<std::uint32_t, 4> counter, keyAndMode;
};
} // namespace

TEST_F(SmsFixture, SmsTrialPhiloxMatchesPublishedKnownAnswers) {
    // Random123 tests/kat_vectors at 9545ff6, BSD-3-Clause; see THIRD-PARTY-NOTICES.md.
    using Words = std::array<std::uint32_t, 4>;
    const std::array inputs{
        SmsTrialInput{Words{0, 0, 0, 0}, Words{0, 0, 0, 0}},
        SmsTrialInput{Words{0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU}, Words{0xFFFFFFFFU, 0xFFFFFFFFU, 0, 0}},
        SmsTrialInput{Words{0x243F6A88U, 0x85A308D3U, 0x13198A2EU, 0x03707344U},
                      Words{0xA4093822U, 0x299F31D0U, 0, 0}}};
    const std::array expected{Words{0x6627E8D5U, 0xE169C58DU, 0xBC57AC4CU, 0x9B00DBD8U},
                              Words{0x408F276DU, 0x41C83B0EU, 0xA20BC7C6U, 0x6D5451FDU},
                              Words{0xD16CFE09U, 0x94FDCCEBU, 0x5001E420U, 0x24126EA1U}};
    std::vector<Words> outputs;
    ASSERT_NO_FATAL_FAILURE((runCompute<SmsTrialInput, Words>(inputs, outputs, "sms_trial_test.spv")));
    ASSERT_EQ(outputs.size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i)
        EXPECT_EQ(outputs[i], expected[i]);
}

TEST_F(SmsFixture, SmsTrialCategoricalReciprocalMatchesKnownExpectation) {
    using Words = std::array<std::uint32_t, 4>;
    constexpr std::uint32_t kCount = 65536U;
    std::vector<SmsTrialInput> inputs;
    inputs.reserve(kCount);
    for (std::uint32_t i = 0; i < kCount; ++i) {
        inputs.push_back({Words{0, 0, 0, 0}, Words{i, 0x534D5301U, 1, 0}});
    }
    std::vector<Words> outputs;
    ASSERT_NO_FATAL_FAILURE((runCompute<SmsTrialInput, Words>(inputs, outputs, "sms_trial_test.spv")));
    ASSERT_EQ(outputs.size(), kCount);
    std::array<double, 2> sums{};
    for (const auto& output : outputs) {
        ASSERT_LE(output[3], 2U);
        if (output[3] == 2U) {
            ASSERT_EQ(output[2], 3U);
        } else {
            ASSERT_EQ(output[2], 1U) << "Reciprocal trial safety bound reached: pending is not an estimate";
            ASSERT_EQ(output[1], 0U);
            sums[output[3]] += output[0];
        }
    }
    // For X=1{initial root=k} K, E[X]=1 and Var[X]=2/p_k-2 (independent trials).
    const std::array probabilities{1.0 / 16.0, 3.0 / 16.0};
    for (std::size_t root = 0; root < sums.size(); ++root) {
        const double mean = sums[root] / kCount;
        const double sixSigma = 6.0 * std::sqrt((2.0 / probabilities[root] - 2.0) / kCount);
        EXPECT_NEAR(mean, 1.0, sixSigma) << "root " << root;
        RecordProperty("root_" + std::to_string(root) + "_mean", std::to_string(mean));
    }
}

namespace {
struct SmsGeometryInput {
    sm::float4 receiver, light;
    std::array<sm::float4, 14> vertexData{};
    sm::float4 lightDu{1, 0, 0, 0}, lightDv{0, 1, 0, 0};
};
struct SmsGeometryOutput {
    sm::float4 value, status;
};
static_assert(sizeof(SmsGeometryInput) == 288 && sizeof(SmsGeometryOutput) == 32);

void setManifoldVertex(SmsGeometryInput& input,
                       std::size_t i,
                       sm::float3 position,
                       sm::float3 normal,
                       float eta = 1.0F,
                       float event = 0.0F,
                       sm::float3 dndu = sm::float3(0),
                       sm::float3 dndv = sm::float3(0)) {
    const auto base = i * 7;
    input.vertexData[base] = sm::float4(position, eta);
    input.vertexData[base + 1] = sm::float4(normal, event);
    input.vertexData[base + 2] = sm::float4(normal, 0);
    input.vertexData[base + 3] = {1, 0, 0, 0};
    input.vertexData[base + 4] = {0, 1, 0, 0};
    input.vertexData[base + 5] = sm::float4(dndu, 0);
    input.vertexData[base + 6] = sm::float4(dndv, 0);
}

SmsGeometryInput mirrorGeometry() {
    SmsGeometryInput input{};
    input.receiver = {-1, 0, 2, 1};
    input.light = {2, 0.5F, 1, 0};
    setManifoldVertex(input, 0, {1, 1.0F / 3.0F, 0}, {0, 0, 1});
    return input;
}

void expectGeometric(const SmsGeometryOutput& output, double expected) {
    ASSERT_EQ(output.status.x, 1.0F);
    EXPECT_NEAR(output.value.x, expected, std::abs(expected) * 2.0e-5);
}
} // namespace

TEST_F(SmsFixture, SmsGeometryMirrorMatchesVirtualEmitterAndCoordinateChanges) {
    const auto base = mirrorGeometry();
    const double distance2 = 3.0 * 3.0 + 0.5 * 0.5 + 3.0 * 3.0;
    const double expected = 3.0 / std::pow(distance2, 1.5);
    std::array inputs{base, base, base, base, base};
    // Rigid rotation, arbitrary surface tangent reparameterization, uniform scale.
    inputs[1].receiver = sm::float4(rotate(xyz(base.receiver)), 1);
    inputs[1].light = sm::float4(rotate(xyz(base.light)), 0);
    for (std::size_t i = 0; i < 7; ++i)
        inputs[1].vertexData[i] = sm::float4(rotate(xyz(base.vertexData[i])), base.vertexData[i].w);
    inputs[1].lightDu = sm::float4(rotate({1, 0, 0}), 0);
    inputs[1].lightDv = sm::float4(rotate({0, 1, 0}), 0);
    inputs[2].vertexData[3] = {2, 0.3F, 0, 0};
    inputs[2].vertexData[4] = {0.2F, 0.7F, 0, 0};
    inputs[3].receiver = sm::float4(xyz(base.receiver) * 3.0F, 1);
    inputs[3].light = sm::float4(xyz(base.light) * 3.0F, 0);
    inputs[3].vertexData[0] = sm::float4(xyz(base.vertexData[0]) * 3.0F, 1);
    const sm::float3 offset(3, -4, 2);
    inputs[4].receiver = sm::float4(xyz(base.receiver) + offset, 1);
    inputs[4].light = sm::float4(xyz(base.light) + offset, 0);
    inputs[4].vertexData[0] = sm::float4(xyz(base.vertexData[0]) + offset, 1);
    std::vector<SmsGeometryOutput> outputs;
    ASSERT_NO_FATAL_FAILURE(
        (runCompute<SmsGeometryInput, SmsGeometryOutput>(inputs, outputs, "sms_geometry_test.spv")));
    ASSERT_EQ(outputs.size(), inputs.size());
    for (std::size_t i = 0; i < inputs.size(); ++i)
        expectGeometric(outputs[i], i == 3 ? expected / 9.0 : expected);
}

TEST_F(SmsFixture, SmsGeometryTwoInterfaceSlabMatchesIndependentSnellAreaFormula) {
    constexpr double h0 = 2, h2 = 3, depth = 0.8, eta = 1.5;
    std::vector<SmsGeometryInput> inputs;
    std::vector<double> expected;
    for (double degrees : {0.0, 25.0, 55.0}) {
        const double theta = degrees * std::numbers::pi / 180.0;
        const double q = std::sin(theta), inside = std::asin(q / eta);
        const sm::float3 p1(static_cast<float>(h0 * std::tan(theta)), 0, 0);
        const auto p2 = p1 + sm::float3(static_cast<float>(depth * std::tan(inside)), 0, -static_cast<float>(depth));
        SmsGeometryInput input{};
        input.receiver = {0, 0, static_cast<float>(h0), 2};
        input.light =
            sm::float4(p2 + sm::float3(static_cast<float>(h2 * std::tan(theta)), 0, -static_cast<float>(h2)), 0);
        setManifoldVertex(input, 0, p1, {0, 0, 1}, static_cast<float>(eta), 1);
        setManifoldVertex(input, 1, p2, {0, 0, -1}, static_cast<float>(1.0 / eta), 1);
        inputs.push_back(input);
        if (degrees == 0)
            expected.push_back(1.0 / std::pow(h0 + h2 + depth / eta, 2));
        else {
            const double radius = (h0 + h2) * std::tan(theta) + depth * std::tan(inside);
            const double derivative =
                (h0 + h2) / std::pow(std::cos(theta), 3) + depth / eta / std::pow(std::cos(inside), 3);
            expected.push_back(q / (std::cos(theta) * radius * derivative));
        }
    }
    std::vector<SmsGeometryOutput> outputs;
    ASSERT_NO_FATAL_FAILURE(
        (runCompute<SmsGeometryInput, SmsGeometryOutput>(inputs, outputs, "sms_geometry_test.spv")));
    ASSERT_EQ(outputs.size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i)
        expectGeometric(outputs[i], expected[i]);
}

TEST_F(SmsFixture, SmsGeometryCurvatureMatchesParaxialMirrorOptics) {
    std::vector<SmsGeometryInput> inputs;
    for (float radius : {1.0F, -10.0F}) {
        SmsGeometryInput input{};
        input.receiver = {0, 0, 2, 1};
        input.light = {0, 0, 3, 0};
        setManifoldVertex(input, 0, {0, 0, 0}, {0, 0, 1}, 1, 0, {1.0F / radius, 0, 0}, {0, 1.0F / radius, 0});
        inputs.push_back(input);
    }
    std::vector<SmsGeometryOutput> outputs;
    ASSERT_NO_FATAL_FAILURE(
        (runCompute<SmsGeometryInput, SmsGeometryOutput>(inputs, outputs, "sms_geometry_test.spv")));
    ASSERT_EQ(outputs.size(), 2U);
    expectGeometric(outputs[0], 1.0 / (17.0 * 17.0));
    expectGeometric(outputs[1], 1.0 / (3.8 * 3.8));
}

TEST_F(SmsFixture, SmsGeometryInvalidAndSingularPathsAreExplicitFailures) {
    std::array inputs{mirrorGeometry(), mirrorGeometry(), mirrorGeometry()};
    inputs[0].receiver.w = 0;
    inputs[1].vertexData[1].w = 1; // Wrong-side "transmission" is not an admissible root.
    inputs[1].vertexData[0].w = 1.5F;
    inputs[2].receiver = {0, 0, 2, 1};
    inputs[2].light = {0, 0, 2, 0};
    setManifoldVertex(inputs[2], 0, {0, 0, 0}, {0, 0, 1}, 1, 0, {-0.5F, 0, 0}, {0, -0.5F, 0}); // Exact conjugate focus:
                                                                                               // singular Jacobian.
    std::vector<SmsGeometryOutput> outputs;
    ASSERT_NO_FATAL_FAILURE(
        (runCompute<SmsGeometryInput, SmsGeometryOutput>(inputs, outputs, "sms_geometry_test.spv")));
    ASSERT_EQ(outputs.size(), inputs.size());
    for (const auto& output : outputs) {
        EXPECT_EQ(output.status.x, 0.0F);
        EXPECT_TRUE(std::isfinite(output.value.x));
    }
}

TEST_F(SmsFixture, SmsInterfaceSnellFresnelAndTirAreSideCorrect) {
    std::array<SmsGeometryInput, 5> inputs{};
    for (auto& input : inputs) {
        input.light.w = 1;
        input.receiver = {0, 0, 1, 1}; // transmission
        input.vertexData[0] = {0, 0, 1, 2.0F / 3.0F};
    }
    inputs[1].vertexData[0].w = 1.5F;
    inputs[2].receiver = {0.5F, 0, std::sqrt(0.75F), 1};
    inputs[3].receiver = {std::sqrt(0.75F), 0, 0.5F, 1};
    inputs[3].vertexData[0].w = 1.5F; // TIR transmission must FAIL, not become reflection.
    inputs[4] = inputs[3];
    inputs[4].receiver.w = 0; // Requested reflection under TIR remains valid.
    std::vector<SmsGeometryOutput> outputs;
    ASSERT_NO_FATAL_FAILURE(
        (runCompute<SmsGeometryInput, SmsGeometryOutput>(inputs, outputs, "sms_geometry_test.spv")));
    ASSERT_EQ(outputs.size(), inputs.size());
    for (auto i : {0U, 1U, 2U, 4U})
        ASSERT_EQ(outputs[i].status.x, 1.0F);
    EXPECT_NEAR(outputs[0].value.w, 0.96F * 4.0F / 9.0F, 1.0e-6F);
    EXPECT_NEAR(outputs[1].value.w, 0.96F * 9.0F / 4.0F, 1.0e-6F);
    EXPECT_NEAR(outputs[0].value.w * outputs[1].value.w, 0.96F * 0.96F, 1.0e-6F);
    EXPECT_NEAR(outputs[2].value.x, -1.0F / 3.0F, 1.0e-6F);
    EXPECT_NEAR(outputs[2].value.z, -std::sqrt(8.0F / 9.0F), 1.0e-6F);
    EXPECT_EQ(outputs[3].status.x, 0.0F);
    EXPECT_EQ(outputs[3].value.w, 0.0F);
    EXPECT_NEAR(outputs[4].value.w, 1.0F, 1.0e-6F);
}

namespace {
struct SmsTestScene {
    std::unique_ptr<harmonia::scene_rt::TriangleMesh> mesh;
    harmonia::rt::AccelerationStructure tlas;
};

std::expected<SmsTestScene, VkResult>
createSmsTestScene(const harmonia::DeviceContext& ctx, const harmonia::core::CommandPool& pool, bool blocker = false) {
    harmonia::scene::MeshData data;
    const std::array heights{0.0F, -0.8F, 1.0F};
    const std::size_t planes = blocker ? 3U : 2U;
    for (std::size_t plane = 0; plane < planes; ++plane) {
        const auto base = static_cast<std::uint32_t>(data.vertices.size());
        for (auto xy : {sm::float2(-10, -10), sm::float2(10, -10), sm::float2(10, 10), sm::float2(-10, 10)}) {
            harmonia::GpuVertex vertex{};
            vertex.position = {xy.x, xy.y, heights[plane]};
            vertex.normal = {0, 0, plane == 1 ? -1.0F : 1.0F};
            data.vertices.push_back(vertex);
        }
        if (plane == 1)
            data.indices.insert(data.indices.end(), {base, base + 2, base + 1, base, base + 3, base + 2});
        else
            data.indices.insert(data.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }
    auto mesh = harmonia::scene_rt::TriangleMesh::create(ctx, pool, std::move(data), {}, "test.sms.planes");
    if (!mesh)
        return std::unexpected(mesh.error());
    if (const auto result = (*mesh)->buildBlas(ctx, pool); result != VK_SUCCESS)
        return std::unexpected(result);
    SmsTestScene scene;
    scene.mesh = std::move(*mesh);
    const std::array instances{scene.mesh->makeInstance(7U, harmonia::scene::Transform{})};
    VkDeviceAddress address{};
    if (const auto result = harmonia::rt::buildTlas(ctx, pool, instances, scene.tlas, address); result != VK_SUCCESS)
        return std::unexpected(result);
    return scene;
}

struct SmsSolveInput {
    sm::float4 receiver, light, seed;
};
struct SmsSolveOutput {
    sm::float4 first, second;
};
struct SmsRootInput {
    sm::float4 a0, a1, b0, b1;
    std::array<std::uint32_t, 4> modes;
};
static_assert(sizeof(SmsSolveInput) == 48 && sizeof(SmsSolveOutput) == 32 && sizeof(SmsRootInput) == 80);
} // namespace

TEST_F(SmsFixture, SmsProjectedMirrorRootsUseHardwareReprojection) {
    auto scene = createSmsTestScene(deviceCtx(), commandPool());
    ASSERT_TRUE(scene.has_value()) << static_cast<int>(scene.error());
    std::vector<SmsSolveInput> inputs;
    for (auto seed : {sm::float3(0, 0, 0), sm::float3(0.8F, 0.2F, 0), sm::float3(1.2F, 0.5F, 0)})
        inputs.push_back({{-1, 0, 2, 1}, {2, 0.5F, 1, 0}, sm::float4(seed, 1)});
    std::vector<SmsSolveOutput> outputs;
    ASSERT_NO_FATAL_FAILURE(
        (runCompute<SmsSolveInput, SmsSolveOutput>(inputs, outputs, "sms_projected_test.spv", scene->tlas.handle())));
    ASSERT_EQ(outputs.size(), inputs.size());
    for (const auto& output : outputs) {
        EXPECT_EQ(output.first.w, 1.0F);
        EXPECT_LE(sm::length(xyz(output.first) - sm::float3(1, 1.0F / 3.0F, 0)), 3.0e-4F);
        EXPECT_GT(output.second.w, 1.0F) << "Non-root starts must execute scene reprojection";
    }
}

TEST_F(SmsFixture, SmsProjectedSlabRootsMatchIndependentSnellSolution) {
    auto scene = createSmsTestScene(deviceCtx(), commandPool());
    ASSERT_TRUE(scene.has_value());
    const double theta = 25.0 * std::numbers::pi / 180.0;
    const double inside = std::asin(std::sin(theta) / 1.5);
    const sm::float3 first(static_cast<float>(2.0 * std::tan(theta)), 0, 0);
    const auto second = first + sm::float3(static_cast<float>(0.8 * std::tan(inside)), 0, -0.8F);
    const auto light = second + sm::float3(static_cast<float>(3.0 * std::tan(theta)), 0, -3);
    std::vector<SmsSolveInput> inputs;
    for (auto seed : {sm::float3(0, 0, 0), sm::float3(0.5F, 0.2F, 0), sm::float3(1.3F, -0.3F, 0)})
        inputs.push_back({{0, 0, 2, 2}, sm::float4(light, 0), sm::float4(seed, 1.5F)});
    std::vector<SmsSolveOutput> outputs;
    ASSERT_NO_FATAL_FAILURE(
        (runCompute<SmsSolveInput, SmsSolveOutput>(inputs, outputs, "sms_projected_test.spv", scene->tlas.handle())));
    ASSERT_EQ(outputs.size(), inputs.size());
    for (const auto& output : outputs) {
        EXPECT_EQ(output.first.w, 1.0F);
        EXPECT_LE(sm::length(xyz(output.first) - first), 3.0e-4F);
        EXPECT_LE(sm::length(xyz(output.second) - second), 3.0e-4F);
        EXPECT_GT(output.second.w, 1.0F);
    }
}

TEST_F(SmsFixture, SmsProjectedMissTopologyAndTirAreRejected) {
    auto scene = createSmsTestScene(deviceCtx(), commandPool());
    ASSERT_TRUE(scene.has_value());
    const std::array inputs{SmsSolveInput{{-1, 0, 2, 1}, {2, 0.5F, 1, 0}, {100, 0, 0, 1}},
                            SmsSolveInput{{0, 0, 2, 2}, {2, 0, -3.8F, 0}, {5, 0, 0, 2.0F / 3.0F}},
                            SmsSolveInput{{-1, 0, 2, 1}, {2, 0.5F, -3.8F, 0}, {0, 0, 0, 1}},
                            SmsSolveInput{{-1, 0, 2, 1}, {2, 0.5F, 1, 1}, {1.001F, 1.0F / 3.0F, 0, 1}}};
    std::vector<SmsSolveOutput> outputs;
    ASSERT_NO_FATAL_FAILURE(
        (runCompute<SmsSolveInput, SmsSolveOutput>(inputs, outputs, "sms_projected_test.spv", scene->tlas.handle())));
    ASSERT_EQ(outputs.size(), inputs.size());
    for (const auto& output : outputs)
        EXPECT_EQ(output.first.w, 0.0F);
    auto blocked = createSmsTestScene(deviceCtx(), commandPool(), true);
    ASSERT_TRUE(blocked.has_value());
    const std::array blockedInput{SmsSolveInput{{-1, 0, 2, 1}, {2, 0.5F, 1, 0}, {0, 0, 0, 1}}};
    ASSERT_NO_FATAL_FAILURE((runCompute<SmsSolveInput, SmsSolveOutput>(
        blockedInput, outputs, "sms_projected_test.spv", blocked->tlas.handle())));
    ASSERT_EQ(outputs.size(), 1U);
    EXPECT_EQ(outputs[0].first.w, 0.0F);
}

TEST_F(SmsFixture, SmsRootIdentityRejectsDistinctDirectionsTopologyAndSides) {
    const SmsRootInput base{{0, 0, 0, 1.0e-4F}, {0, 0, -1, 0}, {0, 0, 0, 0}, {0, 0, -1, 0}, {1, 1, 0, 0}};
    std::array inputs{base, base, base, base, base, base, base, base, base, base};
    inputs[1].b0.x = 5.0e-5F; // solver jitter within the declared tolerance
    inputs[2].b0.x = 2.0e-4F; // distinct even though dot(unitA,unitB) may round to 1
    inputs[3].modes[2] = 1;
    inputs[4].modes[2] = 2;
    inputs[5].modes[2] = 4;
    inputs[6].modes[1] = 2;
    inputs[7].modes = {2, 2, 8, 0};
    inputs[8].modes[0] = 0;
    inputs[9].b0.x = std::numeric_limits<float>::quiet_NaN();
    using Words = std::array<std::uint32_t, 4>;
    std::vector<Words> outputs;
    ASSERT_NO_FATAL_FAILURE((runCompute<SmsRootInput, Words>(inputs, outputs, "sms_root_test.spv")));
    ASSERT_EQ(outputs.size(), inputs.size());
    for (std::size_t i = 0; i < inputs.size(); ++i)
        EXPECT_EQ(outputs[i][0], i < 2 ? 1U : 0U) << i;
}

TEST_F(SmsFixture, SmsMeasureDeltaEventsDoNotExposeContinuousNeeDensity) {
    using Words = std::array<std::uint32_t, 4>;
    std::vector<Words> inputs;
    for (std::uint32_t mode = 0; mode < 12; ++mode)
        inputs.push_back(Words{mode, 0, 0, 0});
    std::vector<sm::float4> outputs;
    ASSERT_NO_FATAL_FAILURE((runCompute<Words, sm::float4>(inputs, outputs, "sms_measure_test.spv")));
    ASSERT_EQ(outputs.size(), inputs.size());
    for (auto i : {0U, 2U, 3U, 6U, 10U, 11U}) {
        SCOPED_TRACE(i);
        EXPECT_EQ(outputs[i].x, 0.0F) << "Delta-only base must not add a continuous NEE lobe";
        EXPECT_EQ(outputs[i].y, 0.0F) << "Continuous PDF query must not report a delta event mass";
        EXPECT_EQ(outputs[i].z, 1.0F);
        EXPECT_GT(outputs[i].w, 0.0F) << "Delta sampler must retain its established positive PDF sentinel";
    }
    for (auto i : {1U, 4U, 5U, 7U, 8U, 9U}) {
        EXPECT_GT(outputs[i].x, 0.0F) << i;
        EXPECT_GT(outputs[i].y, 0.0F) << i;
    }
}

TEST_F(SmsFixture, SmsQueueGridAndPaddingMatchWideIntegerOracle) {
    using Words = std::array<std::uint32_t, 4>;
    std::vector<Words> inputs;
    for (auto count : {0U, 1U, 63U, 64U, 65U, 65535U * 64U, 65535U * 64U + 1U, 0x7FFFFFFFU, 0x80000000U, 0xFFFFFFFFU}) {
        const auto groups = (std::uint64_t{count} + 63) / 64;
        const auto gx = std::min<std::uint64_t>(groups, 65535);
        inputs.push_back({count, 0, 0, 0});
        if (count != 0) {
            const auto group = groups - 1;
            inputs.push_back({count,
                              static_cast<std::uint32_t>(group % gx),
                              static_cast<std::uint32_t>(group / gx),
                              (count - 1) % 64});
            inputs.push_back(
                {count, static_cast<std::uint32_t>(group % gx), static_cast<std::uint32_t>(group / gx), 63});
            // First padded group and last grid group must not wrap at UINT_MAX.
            const auto gy = (groups + gx - 1) / gx;
            inputs.push_back(
                {count, static_cast<std::uint32_t>(groups % gx), static_cast<std::uint32_t>(groups / gx), 0});
            inputs.push_back({count, static_cast<std::uint32_t>(gx - 1), static_cast<std::uint32_t>(gy - 1), 63});
        }
    }
    std::vector<Words> outputs;
    ASSERT_NO_FATAL_FAILURE((runCompute<Words, Words>(inputs, outputs, "sms_queue_grid_test.spv")));
    ASSERT_EQ(outputs.size(), inputs.size());
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        const auto count = std::uint64_t{inputs[i][0]};
        const auto groups = (count + 63) / 64;
        const auto gx = std::min<std::uint64_t>(groups, 65535);
        const auto gy = gx == 0 ? 1 : (groups + gx - 1) / gx;
        const auto index = (std::uint64_t{inputs[i][2]} * gx + inputs[i][1]) * 64 + inputs[i][3];
        EXPECT_EQ(outputs[i][0], gx) << i;
        EXPECT_EQ(outputs[i][1], gy) << i;
        EXPECT_EQ(outputs[i][2], index < count ? 1U : 0U) << i;
        if (index < count)
            EXPECT_EQ(outputs[i][3], index) << i;
    }
}

TEST_F(SmsFixture, SmsTransportDomainMatchesSamplerBoundariesAndProposalMass) {
    struct Probe {
        sm::float4 settings;
        sm::uint4 modes;
    };
    static_assert(sizeof(Probe) == 32);
    constexpr float cutoff = 1.0e-5F, tinyWeight = 1.0e-6F;
    const auto below = std::nextafter(cutoff, 0.0F), above = std::nextafter(cutoff, 1.0F);
    const std::array inputs{Probe{{1.0e-8F, 1, 0, 0}, {0, 0, 0, 0}},
                            Probe{{tinyWeight, 1, 0, 0}, {0, 0, 0, 0}},
                            Probe{{std::nextafter(tinyWeight, 0.0F), 1, 0, 0}, {0, 0, 0, 0}},
                            Probe{{1, below, 0, 0}, {1, 0, 0, 0}},
                            Probe{{1, cutoff, 0, 0}, {1, 0, 0, 0}},
                            Probe{{1, above, 0, 0}, {1, 0, 0, 0}},
                            Probe{{1, below, 0, 0}, {0, 0, 0, 0}},
                            Probe{{1, cutoff, 0, 0}, {0, 0, 0, 0}},
                            Probe{{1, above, 0, 0}, {0, 0, 0, 0}},
                            Probe{{1, 1, 1, 1}, {2, 0, 0, 0}},
                            Probe{{1, 1, .5F, 1}, {2, 0, 0, 0}},
                            Probe{{1, 1, 0, std::numeric_limits<float>::quiet_NaN()}, {2, 0, 0, 0}},
                            Probe{{1, 1, .5F, .25F}, {2, 0, 0, 0}},
                            Probe{{1, 1, 0, std::numeric_limits<float>::infinity()}, {2, 0, 0, 0}},
                            Probe{{0, 1, 0, 1}, {2, 0, 0, 0}},
                            Probe{{std::numeric_limits<float>::infinity(), 1, 0, 1}, {2, 0, 0, 0}},
                            Probe{{1, 1, 1, 0}, {0, 1, 1, 0}},
                            Probe{{1, 1, 1, 0}, {0, 1, 1, 1}},
                            Probe{{1, 1, 1.0e-4F, 0}, {0, 1, 1, 0}}};
    std::vector<sm::float4> outputs;
    ASSERT_NO_FATAL_FAILURE((runCompute<Probe, sm::float4>(inputs, outputs, "sms_transport_test.spv")));
    ASSERT_EQ(outputs.size(), inputs.size());
    for (auto i : {0U, 2U}) {
        EXPECT_EQ(outputs[i].x, 0.0F) << "Tiny glass remains ordinary-PT-owned";
        EXPECT_EQ(outputs[i].y, 0.0F);
        EXPECT_GT(outputs[i].z, 0.0F) << "Exclusion must not remove the existing BSDF event";
    }
    EXPECT_NEAR(outputs[0].z, .01F, 1e-7F); // actual invPLobe floor, not a copied CPU sampler
    EXPECT_EQ(outputs[1].x, 1.0F);
    EXPECT_NEAR(outputs[1].y, .04F, 1e-6F);
    EXPECT_EQ(outputs[1].z, 1.0F);
    for (auto i : {3U, 4U, 6U, 7U}) {
        EXPECT_EQ(outputs[i].x, 1.0F);
        EXPECT_EQ(outputs[i].y, 0.0F) << "SMS must retain sampleBSDF's exact grazing zero-event boundary";
        EXPECT_EQ(outputs[i].z, 0.0F);
        EXPECT_EQ(outputs[i].w, 0.0F);
    }
    for (auto i : {5U, 8U}) {
        EXPECT_EQ(outputs[i].x, 1.0F);
        EXPECT_GT(outputs[i].y, 0.0F);
        EXPECT_EQ(outputs[i].z, 1.0F);
        EXPECT_GT(outputs[i].w, 0.0F);
    }
    for (auto i : {9U, 11U, 12U, 13U, 14U, 15U}) {
        EXPECT_EQ(outputs[i].x, 0.0F) << "Unproposable emitter interval/density remains PT-owned: " << i;
        EXPECT_EQ(outputs[i].z, 0.0F);
    }
    EXPECT_EQ(outputs[9].y, 0.0F) << "Repeated fp32 CDF interval must have zero ownership mass";
    EXPECT_EQ(outputs[10].x, 1.0F);
    EXPECT_EQ(outputs[10].y, .5F);
    EXPECT_EQ(outputs[10].z, .5F);
    EXPECT_EQ(outputs[16].x, 1.0F) << "Pure absorption is supported";
    EXPECT_NEAR(outputs[16].y, .96F / 2.25F, 1e-6F) << "Positive-depth interfaces are untinted";
    EXPECT_EQ(outputs[16].z, 1.0F) << "Actual sampler retains its untinted volume-entry weight";
    EXPECT_EQ(outputs[17].x, 0.0F) << "Scattering volumes stay PT-owned";
    EXPECT_EQ(outputs[17].y, 0.0F);
    EXPECT_GT(outputs[17].z, 0.0F);
    EXPECT_EQ(outputs[18].x, 1.0F);
    EXPECT_NEAR(outputs[18].y, .8F * .96F / 2.25F, 1e-6F) << "At the no-volume boundary per-crossing tint remains";
}
