// Component tests: shared denoiser stage behavior and disabled no-op contract.

#include <volk/volk.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <gtest/gtest.h>
#include <slang-math/slang-math.hpp>
#include <vector>

#include "fixtures/VulkanTestFixture.hpp"
#include "harmonia/core/Buffer.hpp"
#include "harmonia/core/Image.hpp"
#include "harmonia/pipeline/PassContext.hpp"
#include "harmonia/pipeline/SceneOutputCopyPass.hpp"

namespace {

[[nodiscard]] std::vector<sm::float4>
readRgbaImage(const harmonia::DeviceContext& deviceCtx, harmonia::CommandPool& commandPool, harmonia::Image& image) {
    const VkDeviceSize byteSize =
        static_cast<VkDeviceSize>(image.extent().width) * image.extent().height * sizeof(sm::float4);
    auto readback = harmonia::Buffer::create(deviceCtx,
                                             byteSize,
                                             VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                             VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                             "test.sceneoutput.readback");
    EXPECT_TRUE(readback.has_value()) << static_cast<int>(readback.error());
    if (!readback) {
        return {};
    }

    auto cmd = commandPool.beginOneShot();
    EXPECT_TRUE(cmd.has_value()) << static_cast<int>(cmd.error());
    if (!cmd) {
        return {};
    }

    image.transition(*cmd,
                     VK_IMAGE_LAYOUT_GENERAL,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                     VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT |
                         VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                     VK_ACCESS_2_TRANSFER_READ_BIT);
    const VkBufferImageCopy region{
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .imageOffset = {0, 0, 0},
        .imageExtent = {image.extent().width, image.extent().height, 1},
    };
    vkCmdCopyImageToBuffer(*cmd, image.handle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback->handle(), 1, &region);
    image.transition(*cmd,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_IMAGE_LAYOUT_GENERAL,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                     VK_ACCESS_2_TRANSFER_READ_BIT,
                     VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                     VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT |
                         VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    EXPECT_EQ(commandPool.endOneShot(*cmd), VK_SUCCESS);

    std::vector<sm::float4> pixels(static_cast<std::size_t>(image.extent().width) * image.extent().height);
    std::memcpy(pixels.data(), readback->mappedData(), static_cast<std::size_t>(byteSize));
    return pixels;
}

void uploadRgbaImage(const harmonia::DeviceContext& deviceCtx,
                     harmonia::CommandPool& commandPool,
                     harmonia::Image& image,
                     const std::vector<sm::float4>& pixels) {
    const VkDeviceSize byteSize =
        static_cast<VkDeviceSize>(image.extent().width) * image.extent().height * sizeof(sm::float4);
    auto staging = harmonia::Buffer::create(deviceCtx,
                                            byteSize,
                                            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                            VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                            "test.sceneoutput.staging.rgba");
    ASSERT_TRUE(staging.has_value()) << static_cast<int>(staging.error());
    staging->uploadData(pixels.data(), byteSize);

    auto cmd = commandPool.beginOneShot();
    ASSERT_TRUE(cmd.has_value()) << static_cast<int>(cmd.error());
    image.transition(*cmd,
                     VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_PIPELINE_STAGE_2_NONE,
                     0,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                     VK_ACCESS_2_TRANSFER_WRITE_BIT);
    const VkBufferImageCopy region{
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .imageOffset = {0, 0, 0},
        .imageExtent = {image.extent().width, image.extent().height, 1},
    };
    vkCmdCopyBufferToImage(*cmd, staging->handle(), image.handle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    image.transition(*cmd,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_GENERAL,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                     VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                     VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT |
                         VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
    ASSERT_EQ(commandPool.endOneShot(*cmd), VK_SUCCESS);
}

void uploadDepthImage(const harmonia::DeviceContext& deviceCtx,
                      harmonia::CommandPool& commandPool,
                      harmonia::Image& image,
                      const std::vector<float>& pixels) {
    const VkDeviceSize byteSize =
        static_cast<VkDeviceSize>(image.extent().width) * image.extent().height * sizeof(float);
    auto staging = harmonia::Buffer::create(deviceCtx,
                                            byteSize,
                                            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                            VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                            "test.sceneoutput.staging.depth");
    ASSERT_TRUE(staging.has_value()) << static_cast<int>(staging.error());
    staging->uploadData(pixels.data(), byteSize);

    auto cmd = commandPool.beginOneShot();
    ASSERT_TRUE(cmd.has_value()) << static_cast<int>(cmd.error());
    image.transition(*cmd,
                     VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_PIPELINE_STAGE_2_NONE,
                     0,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                     VK_ACCESS_2_TRANSFER_WRITE_BIT);
    const VkBufferImageCopy region{
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .imageOffset = {0, 0, 0},
        .imageExtent = {image.extent().width, image.extent().height, 1},
    };
    vkCmdCopyBufferToImage(*cmd, staging->handle(), image.handle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    image.transition(*cmd,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_GENERAL,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                     VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                     VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT |
                         VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
    ASSERT_EQ(commandPool.endOneShot(*cmd), VK_SUCCESS);
}

[[nodiscard]] float luminance(const sm::float4& c) {
    return c.r * 0.2627F + c.g * 0.6780F + c.b * 0.0593F;
}

} // namespace

TEST_F(VulkanFixture, SceneOutputCopyPass_ReducesNoiseAndPreservesDepthEdge) {
    constexpr VkExtent2D kExtent{16U, 8U};
    const VkDeviceSize kRgbaBytes = static_cast<VkDeviceSize>(kExtent.width) * kExtent.height * sizeof(sm::float4);
    static_cast<void>(kRgbaBytes);

    auto hdr = harmonia::Image::create(deviceCtx(),
                                       kExtent,
                                       VK_FORMAT_R32G32B32A32_SFLOAT,
                                       VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                           VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                       VK_IMAGE_ASPECT_COLOR_BIT,
                                       "test.sceneoutput.hdr");
    ASSERT_TRUE(hdr.has_value()) << static_cast<int>(hdr.error());

    auto denoised = harmonia::Image::create(deviceCtx(),
                                            kExtent,
                                            VK_FORMAT_R32G32B32A32_SFLOAT,
                                            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                                VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                            VK_IMAGE_ASPECT_COLOR_BIT,
                                            "test.sceneoutput.denoised");
    ASSERT_TRUE(denoised.has_value()) << static_cast<int>(denoised.error());

    auto gNormal = harmonia::Image::create(deviceCtx(),
                                           kExtent,
                                           VK_FORMAT_R32G32B32A32_SFLOAT,
                                           VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                           VK_IMAGE_ASPECT_COLOR_BIT,
                                           "test.sceneoutput.gnormal");
    ASSERT_TRUE(gNormal.has_value()) << static_cast<int>(gNormal.error());

    auto gDepth = harmonia::Image::create(deviceCtx(),
                                          kExtent,
                                          VK_FORMAT_R32_SFLOAT,
                                          VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                          VK_IMAGE_ASPECT_COLOR_BIT,
                                          "test.sceneoutput.gdepth");
    ASSERT_TRUE(gDepth.has_value()) << static_cast<int>(gDepth.error());

    std::vector<sm::float4> hdrPixels(static_cast<std::size_t>(kExtent.width) * kExtent.height);
    std::vector<sm::float4> normalPixels(static_cast<std::size_t>(kExtent.width) * kExtent.height);
    std::vector<float> depthPixels(static_cast<std::size_t>(kExtent.width) * kExtent.height);
    for (std::uint32_t y = 0; y < kExtent.height; ++y) {
        for (std::uint32_t x = 0; x < kExtent.width; ++x) {
            const std::size_t idx = static_cast<std::size_t>(y) * kExtent.width + x;
            const bool left = x < (kExtent.width / 2U);
            const float base = left ? 0.30F : 1.40F;
            const int pattern = static_cast<int>((x * 17U + y * 29U) % 9U) - 4;
            const float noise = static_cast<float>(pattern) * 0.035F;
            const float value = std::max(base + noise, 0.0F);
            hdrPixels[idx] = sm::float4(value, value, value, 1.0F);
            normalPixels[idx] = left ? sm::float4(0.5F, 0.5F, 1.0F, 0.0F) : sm::float4(1.0F, 0.5F, 0.5F, 0.0F);
            depthPixels[idx] = left ? 1.0F : 4.0F;
        }
    }

    uploadRgbaImage(deviceCtx(), commandPool(), *hdr, hdrPixels);
    uploadRgbaImage(deviceCtx(), commandPool(), *gNormal, normalPixels);
    uploadDepthImage(deviceCtx(), commandPool(), *gDepth, depthPixels);

    auto pass = harmonia::SceneOutputCopyPass::create(deviceCtx(),
                                                      kExtent,
                                                      std::filesystem::path(HARMONIA_SHADER_DIR) / "denoiser.spv",
                                                      harmonia::SceneOutputCopyPass::Settings{
                                                          .strength = 0.75F,
                                                          .iterations = 3U,
                                                          .useHistory = false,
                                                          .historyBlend = 0.1F,
                                                      });
    ASSERT_TRUE(pass.has_value()) << static_cast<int>(pass.error());
    pass->onResize(kExtent);

    auto cmd = commandPool().beginOneShot();
    ASSERT_TRUE(cmd.has_value()) << static_cast<int>(cmd.error());
    const harmonia::PassContext passContext{
        .cmd = *cmd,
        .frameIndex = 0,
        .extent = kExtent,
        .fixedView = true,
        .accumulationResetToken = 1U,
        .hdrBuffer = &*hdr,
        .gNormalView = gNormal->view(),
        .gDepthView = gDepth->view(),
        .denoised = &*denoised,
        .swapchainView = VK_NULL_HANDLE,
        .colorSpace = harmonia::OutputColorSpace::eSDR,
    };
    pass->record(passContext);
    ASSERT_EQ(commandPool().endOneShot(*cmd), VK_SUCCESS);

    const auto denoisedPixels = readRgbaImage(deviceCtx(), commandPool(), *denoised);
    ASSERT_EQ(denoisedPixels.size(), hdrPixels.size());

    float leftMeanIn = 0.0F;
    float leftMeanOut = 0.0F;
    float leftVarIn = 0.0F;
    float leftVarOut = 0.0F;
    float edgeIn = 0.0F;
    float edgeOut = 0.0F;
    const std::uint32_t half = kExtent.width / 2U;
    for (std::uint32_t y = 0; y < kExtent.height; ++y) {
        for (std::uint32_t x = 0; x < half; ++x) {
            const std::size_t idx = static_cast<std::size_t>(y) * kExtent.width + x;
            leftMeanIn += luminance(hdrPixels[idx]);
            leftMeanOut += luminance(denoisedPixels[idx]);
        }
        const std::size_t leftIdx = static_cast<std::size_t>(y) * kExtent.width + (half - 1U);
        const std::size_t rightIdx = static_cast<std::size_t>(y) * kExtent.width + half;
        edgeIn += std::abs(luminance(hdrPixels[rightIdx]) - luminance(hdrPixels[leftIdx]));
        edgeOut += std::abs(luminance(denoisedPixels[rightIdx]) - luminance(denoisedPixels[leftIdx]));
    }
    const float sampleCount = static_cast<float>(half * kExtent.height);
    leftMeanIn /= sampleCount;
    leftMeanOut /= sampleCount;
    edgeIn /= static_cast<float>(kExtent.height);
    edgeOut /= static_cast<float>(kExtent.height);

    for (std::uint32_t y = 0; y < kExtent.height; ++y) {
        for (std::uint32_t x = 0; x < half; ++x) {
            const std::size_t idx = static_cast<std::size_t>(y) * kExtent.width + x;
            const float in = luminance(hdrPixels[idx]) - leftMeanIn;
            const float out = luminance(denoisedPixels[idx]) - leftMeanOut;
            leftVarIn += in * in;
            leftVarOut += out * out;
        }
    }
    const float leftStdIn = std::sqrt(leftVarIn / sampleCount);
    const float leftStdOut = std::sqrt(leftVarOut / sampleCount);

    EXPECT_LT(leftStdOut, leftStdIn * 0.8F) << "denoiser should suppress intra-surface noise";
    EXPECT_GT(edgeOut, edgeIn * 0.65F) << "depth/normal edge should remain visible";
}

TEST_F(VulkanFixture, SceneOutputCopyPass_NoOpWhenDenoiserOutputMissing) {
    constexpr VkExtent2D kExtent{8U, 8U};
    constexpr VkClearColorValue kClear{.float32 = {0.25F, 0.5F, 0.75F, 1.0F}};

    auto hdr = harmonia::Image::create(deviceCtx(),
                                       kExtent,
                                       VK_FORMAT_R32G32B32A32_SFLOAT,
                                       VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                           VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                       VK_IMAGE_ASPECT_COLOR_BIT,
                                       "test.sceneoutput.noop.hdr");
    ASSERT_TRUE(hdr.has_value()) << static_cast<int>(hdr.error());

    auto pass = harmonia::SceneOutputCopyPass::create(
        deviceCtx(), kExtent, std::filesystem::path(HARMONIA_SHADER_DIR) / "denoiser.spv");
    ASSERT_TRUE(pass.has_value()) << static_cast<int>(pass.error());
    pass->onResize(kExtent);

    auto cmd = commandPool().beginOneShot();
    ASSERT_TRUE(cmd.has_value()) << static_cast<int>(cmd.error());
    hdr->transition(*cmd,
                    VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_PIPELINE_STAGE_2_NONE,
                    0,
                    VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT);
    const VkImageSubresourceRange range{
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = 1,
    };
    vkCmdClearColorImage(*cmd, hdr->handle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &kClear, 1, &range);
    hdr->transition(*cmd,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_GENERAL,
                    VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                    VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT |
                        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

    const harmonia::PassContext passContext{
        .cmd = *cmd,
        .frameIndex = 0,
        .extent = kExtent,
        .fixedView = true,
        .accumulationResetToken = 1U,
        .hdrBuffer = &*hdr,
        .gNormalView = VK_NULL_HANDLE,
        .gDepthView = VK_NULL_HANDLE,
        .denoised = nullptr,
        .swapchainView = VK_NULL_HANDLE,
        .colorSpace = harmonia::OutputColorSpace::eSDR,
    };
    pass->record(passContext);
    ASSERT_EQ(commandPool().endOneShot(*cmd), VK_SUCCESS);

    const auto pixels = readRgbaImage(deviceCtx(), commandPool(), *hdr);
    ASSERT_FALSE(pixels.empty());
    const sm::float4 expected(kClear.float32[0], kClear.float32[1], kClear.float32[2], kClear.float32[3]);
    EXPECT_NEAR(pixels[0].r, expected.r, 1e-5F);
    EXPECT_NEAR(pixels[0].g, expected.g, 1e-5F);
    EXPECT_NEAR(pixels[0].b, expected.b, 1e-5F);
    EXPECT_NEAR(pixels[0].a, expected.a, 1e-5F);
}
