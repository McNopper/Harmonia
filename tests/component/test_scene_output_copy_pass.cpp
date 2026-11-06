// Component tests: shared denoiser stage behavior and disabled no-op contract.

#include <volk/volk.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <gtest/gtest.h>
#include <limits>
#include <slang-math/slang-math.hpp>
#include <vector>

#include "fixtures/VulkanTestFixture.hpp"
#include "harmonia/core/Buffer.hpp"
#include "harmonia/core/Image.hpp"
#include "harmonia/core/Logger.hpp"
#include "harmonia/pipeline/PassContext.hpp"
#include "harmonia/pipeline/SceneOutputCopyPass.hpp"

namespace {

[[nodiscard]] std::vector<sm::float4> readRgbaImage(const harmonia::DeviceContext& deviceCtx,
                                                    harmonia::core::CommandPool& commandPool,
                                                    harmonia::core::Image& image) {
    const VkDeviceSize byteSize =
        static_cast<VkDeviceSize>(image.extent().width) * image.extent().height * sizeof(sm::float4);
    auto readback = harmonia::core::Buffer::create(deviceCtx,
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
                     harmonia::core::CommandPool& commandPool,
                     harmonia::core::Image& image,
                     const std::vector<sm::float4>& pixels) {
    const VkDeviceSize byteSize =
        static_cast<VkDeviceSize>(image.extent().width) * image.extent().height * sizeof(sm::float4);
    auto staging = harmonia::core::Buffer::create(deviceCtx,
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
                      harmonia::core::CommandPool& commandPool,
                      harmonia::core::Image& image,
                      const std::vector<float>& pixels) {
    const VkDeviceSize byteSize =
        static_cast<VkDeviceSize>(image.extent().width) * image.extent().height * sizeof(float);
    auto staging = harmonia::core::Buffer::create(deviceCtx,
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

    auto hdr = harmonia::core::Image::create(deviceCtx(),
                                             kExtent,
                                             VK_FORMAT_R32G32B32A32_SFLOAT,
                                             VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                             VK_IMAGE_ASPECT_COLOR_BIT,
                                             "test.sceneoutput.hdr");
    ASSERT_TRUE(hdr.has_value()) << static_cast<int>(hdr.error());

    auto denoised = harmonia::core::Image::create(deviceCtx(),
                                                  kExtent,
                                                  VK_FORMAT_R32G32B32A32_SFLOAT,
                                                  VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                                      VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                                  VK_IMAGE_ASPECT_COLOR_BIT,
                                                  "test.sceneoutput.denoised");
    ASSERT_TRUE(denoised.has_value()) << static_cast<int>(denoised.error());

    auto gNormal = harmonia::core::Image::create(deviceCtx(),
                                                 kExtent,
                                                 VK_FORMAT_R32G32B32A32_SFLOAT,
                                                 VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                                 VK_IMAGE_ASPECT_COLOR_BIT,
                                                 "test.sceneoutput.gnormal");
    ASSERT_TRUE(gNormal.has_value()) << static_cast<int>(gNormal.error());

    auto gDepth = harmonia::core::Image::create(deviceCtx(),
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

    auto pass =
        harmonia::pipeline::SceneOutputCopyPass::create(deviceCtx(),
                                                        kExtent,
                                                        std::filesystem::path(HARMONIA_SHADER_DIR) / "denoiser.spv",
                                                        harmonia::pipeline::SceneOutputCopyPass::Settings{
                                                            .strength = 0.75F,
                                                            .iterations = 3U,
                                                            .useHistory = false,
                                                            .historyBlend = 0.1F,
                                                        });
    ASSERT_TRUE(pass.has_value()) << static_cast<int>(pass.error());
    pass->onResize(kExtent);

    auto cmd = commandPool().beginOneShot();
    ASSERT_TRUE(cmd.has_value()) << static_cast<int>(cmd.error());
    const harmonia::pipeline::PassContext passContext{
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
        .colorSpace = harmonia::utils::OutputColorSpace::eSDR,
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

    auto hdr = harmonia::core::Image::create(deviceCtx(),
                                             kExtent,
                                             VK_FORMAT_R32G32B32A32_SFLOAT,
                                             VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                             VK_IMAGE_ASPECT_COLOR_BIT,
                                             "test.sceneoutput.noop.hdr");
    ASSERT_TRUE(hdr.has_value()) << static_cast<int>(hdr.error());

    auto pass = harmonia::pipeline::SceneOutputCopyPass::create(
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

    const harmonia::pipeline::PassContext passContext{
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
        .colorSpace = harmonia::utils::OutputColorSpace::eSDR,
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

// DN3 convergence receipt (branch dn3/converging-blend): the shipped converging
// blend (constant-W, kConvRef = 256, shipped v0.7.9) must fade to identity as
// the reservoir sample count M grows. MC noise amplitude scales as 1/sqrt(M),
// so the denoised-vs-truth error must decrease toward the noise floor - the
// property the fixed-radius atrous core structurally lacked.
TEST_F(VulkanFixture, SceneOutputCopyPass_DN3BlendConvergesWithSampleCount) {
    constexpr VkExtent2D kExtent{16U, 8U};
    constexpr float kMu = 0.5F;
    constexpr std::array<float, 4> kSampleCounts{1.0F, 16.0F, 256.0F, 4096.0F};

    auto hdr = harmonia::core::Image::create(deviceCtx(),
                                             kExtent,
                                             VK_FORMAT_R32G32B32A32_SFLOAT,
                                             VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                             VK_IMAGE_ASPECT_COLOR_BIT,
                                             "test.dn3.hdr");
    ASSERT_TRUE(hdr.has_value()) << static_cast<int>(hdr.error());
    auto denoised = harmonia::core::Image::create(deviceCtx(),
                                                  kExtent,
                                                  VK_FORMAT_R32G32B32A32_SFLOAT,
                                                  VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                                      VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                                  VK_IMAGE_ASPECT_COLOR_BIT,
                                                  "test.dn3.denoised");
    ASSERT_TRUE(denoised.has_value()) << static_cast<int>(denoised.error());
    auto gNormal = harmonia::core::Image::create(deviceCtx(),
                                                 kExtent,
                                                 VK_FORMAT_R32G32B32A32_SFLOAT,
                                                 VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                                 VK_IMAGE_ASPECT_COLOR_BIT,
                                                 "test.dn3.gnormal");
    ASSERT_TRUE(gNormal.has_value()) << static_cast<int>(gNormal.error());
    auto gDepth = harmonia::core::Image::create(deviceCtx(),
                                                kExtent,
                                                VK_FORMAT_R32_SFLOAT,
                                                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                                VK_IMAGE_ASPECT_COLOR_BIT,
                                                "test.dn3.gdepth");
    ASSERT_TRUE(gDepth.has_value()) << static_cast<int>(gDepth.error());

    std::vector<sm::float4> normalPixels(static_cast<std::size_t>(kExtent.width) * kExtent.height);
    std::vector<float> depthPixels(static_cast<std::size_t>(kExtent.width) * kExtent.height);
    for (std::uint32_t y = 0; y < kExtent.height; ++y) {
        for (std::uint32_t x = 0; x < kExtent.width; ++x) {
            const std::size_t idx = static_cast<std::size_t>(y) * kExtent.width + x;
            normalPixels[idx] = sm::float4(0.5F, 0.5F, 1.0F, 0.0F);
            depthPixels[idx] = 2.0F;
        }
    }
    uploadRgbaImage(deviceCtx(), commandPool(), *gNormal, normalPixels);
    uploadDepthImage(deviceCtx(), commandPool(), *gDepth, depthPixels);

    auto pass =
        harmonia::pipeline::SceneOutputCopyPass::create(deviceCtx(),
                                                        kExtent,
                                                        std::filesystem::path(HARMONIA_SHADER_DIR) / "denoiser.spv",
                                                        harmonia::pipeline::SceneOutputCopyPass::Settings{
                                                            .strength = 0.75F,
                                                            .iterations = 3U,
                                                            .useHistory = false,
                                                            .historyBlend = 0.1F,
                                                        });
    ASSERT_TRUE(pass.has_value()) << static_cast<int>(pass.error());
    pass->onResize(kExtent);

    std::vector<sm::float4> hdrPixels(static_cast<std::size_t>(kExtent.width) * kExtent.height);
    double prevErr = std::numeric_limits<double>::max();
    double firstErr = -1.0;
    double firstInputErr = -1.0;
    bool first = true;
    for (const float sampleCount : kSampleCounts) {
        const float noiseAmp = 0.12F / std::sqrt(sampleCount); // MC standard error ~ 1/sqrt(M)
        double inputErrSum = 0.0;
        for (std::uint32_t y = 0; y < kExtent.height; ++y) {
            for (std::uint32_t x = 0; x < kExtent.width; ++x) {
                const std::size_t idx = static_cast<std::size_t>(y) * kExtent.width + x;
                const int pattern = static_cast<int>((x * 17U + y * 29U) % 9U) - 4;
                const float value = std::max(kMu + static_cast<float>(pattern) * noiseAmp, 0.0F);
                hdrPixels[idx] = sm::float4(value, value, value, sampleCount); // alpha = M
                inputErrSum += std::abs(static_cast<double>(value) - static_cast<double>(kMu));
            }
        }
        uploadRgbaImage(deviceCtx(), commandPool(), *hdr, hdrPixels);

        auto cmd = commandPool().beginOneShot();
        ASSERT_TRUE(cmd.has_value()) << static_cast<int>(cmd.error());
        const harmonia::pipeline::PassContext passContext{
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
            .colorSpace = harmonia::utils::OutputColorSpace::eSDR,
        };
        pass->record(passContext);
        ASSERT_EQ(commandPool().endOneShot(*cmd), VK_SUCCESS);

        const auto denoisedPixels = readRgbaImage(deviceCtx(), commandPool(), *denoised);
        ASSERT_EQ(denoisedPixels.size(), hdrPixels.size());
        const auto pixelCount = static_cast<double>(kExtent.width) * static_cast<double>(kExtent.height);
        double outputErrSum = 0.0;
        for (std::uint32_t y = 0; y < kExtent.height; ++y) {
            for (std::uint32_t x = 0; x < kExtent.width; ++x) {
                const std::size_t idx = static_cast<std::size_t>(y) * kExtent.width + x;
                const float luma =
                    0.2627F * denoisedPixels[idx].r + 0.6780F * denoisedPixels[idx].g + 0.0593F * denoisedPixels[idx].b;
                outputErrSum += std::abs(static_cast<double>(luma) - static_cast<double>(kMu));
            }
        }
        const double inputErr = inputErrSum / pixelCount;
        const double outputErr = outputErrSum / pixelCount;
        harmonia::core::Logger::info(
            "DN3 convergence: M={:.0f} input_err {:.5f} denoised_err {:.5f}", sampleCount, inputErr, outputErr);
        if (first) {
            EXPECT_LT(outputErr, inputErr) << "at M=1 the converging blend must denoise";
            firstErr = outputErr;
            firstInputErr = inputErr;
            first = false;
        } else {
            // Constant-W crossover lag: near the optimal switch point the blend
            // lags the ideal per-pixel switch by a small bounded amount (this
            // lag is what the paper's per-pixel Eq. 7 estimator removes).
            EXPECT_LE(outputErr, prevErr + 1.0e-3) << "constant-W crossover lag must stay bounded";
        }
        prevErr = outputErr;
    }
    EXPECT_LT(prevErr, firstErr) << "final error must sit below the M=1 error - convergence receipt";
    EXPECT_LT(firstErr, firstInputErr) << "M=1 denoised error must beat the raw noisy input error";
}
// DN3-M2 receipt: the paper-exact converging blend (Eq. 2 + Eq. 14 SURE weight)
// must close the constant-W crossover lag. Both arms run the identical noise
// realization per M (deterministic pattern, amplitude 1/sqrt(M)); the converging
// arm chains 3 frames per M (frameSampleIndex M-2, M-1, M; stable reset token)
// so the Eq. 7 curve gets its two variance points from bindings 6/7.
TEST_F(VulkanFixture, SceneOutputCopyPass_DN3M2PaperBlendClosesCrossoverLag) {
    constexpr VkExtent2D kExtent{16U, 8U};
    constexpr float kMu = 0.5F;
    constexpr std::array<float, 4> kSampleCounts{1.0F, 16.0F, 256.0F, 4096.0F};

    auto makeImage = [&](const char* name, VkFormat format) {
        return harmonia::core::Image::create(deviceCtx(),
                                             kExtent,
                                             format,
                                             VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                             VK_IMAGE_ASPECT_COLOR_BIT,
                                             name);
    };
    auto hdrCW = makeImage("test.dn3m2.hdrCW", VK_FORMAT_R32G32B32A32_SFLOAT);
    auto hdrCV = makeImage("test.dn3m2.hdrCV", VK_FORMAT_R32G32B32A32_SFLOAT);
    auto denoisedCW = makeImage("test.dn3m2.denoisedCW", VK_FORMAT_R32G32B32A32_SFLOAT);
    auto denoisedCV = makeImage("test.dn3m2.denoisedCV", VK_FORMAT_R32G32B32A32_SFLOAT);
    auto gNormal = makeImage("test.dn3m2.gnormal", VK_FORMAT_R32G32B32A32_SFLOAT);
    auto gDepth = harmonia::core::Image::create(deviceCtx(),
                                                kExtent,
                                                VK_FORMAT_R32_SFLOAT,
                                                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                                VK_IMAGE_ASPECT_COLOR_BIT,
                                                "test.dn3m2.gdepth");
    ASSERT_TRUE(hdrCW.has_value() && hdrCV.has_value() && denoisedCW.has_value() && denoisedCV.has_value() &&
                gNormal.has_value() && gDepth.has_value());

    std::vector<sm::float4> normalPixels(static_cast<std::size_t>(kExtent.width) * kExtent.height);
    std::vector<float> depthPixels(static_cast<std::size_t>(kExtent.width) * kExtent.height);
    for (std::uint32_t y = 0; y < kExtent.height; ++y) {
        for (std::uint32_t x = 0; x < kExtent.width; ++x) {
            const std::size_t idx = static_cast<std::size_t>(y) * kExtent.width + x;
            normalPixels[idx] = sm::float4(0.5F, 0.5F, 1.0F, 0.0F);
            depthPixels[idx] = 2.0F;
        }
    }
    uploadRgbaImage(deviceCtx(), commandPool(), *gNormal, normalPixels);
    uploadDepthImage(deviceCtx(), commandPool(), *gDepth, depthPixels);

    const std::filesystem::path denoiserSpv = std::filesystem::path(HARMONIA_SHADER_DIR) / "denoiser.spv";
    auto passCW = harmonia::pipeline::SceneOutputCopyPass::create(
        deviceCtx(),
        kExtent,
        denoiserSpv,
        harmonia::pipeline::SceneOutputCopyPassSettings{.strength = 0.75F, .iterations = 3U, .useHistory = true});
    auto passCV = harmonia::pipeline::SceneOutputCopyPass::create(deviceCtx(),
                                                                  kExtent,
                                                                  denoiserSpv,
                                                                  harmonia::pipeline::SceneOutputCopyPassSettings{
                                                                      .strength = 0.75F,
                                                                      .iterations = 3U,
                                                                      .useHistory = true,
                                                                      .historyBlend = 0.1F,
                                                                      .useGradient = true,
                                                                      .convergingMode = true,
                                                                  });
    ASSERT_TRUE(passCW.has_value() && passCV.has_value());
    passCW->onResize(kExtent);
    passCV->onResize(kExtent);

    constexpr std::uint64_t kResetToken = 7U;
    double cwErr4096 = -1.0;
    double cvErr4096 = -1.0;
    double prevCvErr = std::numeric_limits<double>::max();
    bool first = true;
    for (const float sampleCount : kSampleCounts) {
        const float noiseAmp = 0.12F / std::sqrt(sampleCount);
        std::vector<sm::float4> hdrPixels(static_cast<std::size_t>(kExtent.width) * kExtent.height);
        for (std::uint32_t y = 0; y < kExtent.height; ++y) {
            for (std::uint32_t x = 0; x < kExtent.width; ++x) {
                const std::size_t idx = static_cast<std::size_t>(y) * kExtent.width + x;
                const int pattern = static_cast<int>((x * 17U + y * 29U) % 9U) - 4;
                const float value = std::max(kMu + static_cast<float>(pattern) * noiseAmp, 0.0F);
                hdrPixels[idx] = sm::float4(value, value, value, sampleCount); // alpha = M
            }
        }
        uploadRgbaImage(deviceCtx(), commandPool(), *hdrCW, hdrPixels);
        uploadRgbaImage(deviceCtx(), commandPool(), *hdrCV, hdrPixels);

        // constant-W arm: one record (the shipped v0.7.9 blend, no chain).
        auto cmd = commandPool().beginOneShot();
        ASSERT_TRUE(cmd.has_value());
        const harmonia::pipeline::PassContext cwContext{
            .cmd = *cmd,
            .frameIndex = 0,
            .frameSampleIndex = static_cast<std::uint32_t>(sampleCount) - 1U,
            .extent = kExtent,
            .fixedView = true,
            .accumulationResetToken = kResetToken,
            .denoiserResetToken = kResetToken,
            .hdrBuffer = &*hdrCW,
            .gNormalView = gNormal->view(),
            .gDepthView = gDepth->view(),
            .denoised = &*denoisedCW,
            .swapchainView = VK_NULL_HANDLE,
            .colorSpace = harmonia::utils::OutputColorSpace::eSDR,
        };
        passCW->record(cwContext);
        ASSERT_EQ(commandPool().endOneShot(*cmd), VK_SUCCESS);
        const auto cwPixels = readRgbaImage(deviceCtx(), commandPool(), *denoisedCW);

        // converging arm: 3 chained frames (curve points at consecutive counts).
        for (std::uint32_t frame = 0U; frame < 3U; ++frame) {
            const std::uint32_t frameIndex = static_cast<std::uint32_t>(sampleCount) - 1U + frame; // M-2, M-1, M
            auto cmdCv = commandPool().beginOneShot();
            ASSERT_TRUE(cmdCv.has_value());
            const harmonia::pipeline::PassContext cvContext{
                .cmd = *cmdCv,
                .frameIndex = frame,
                .frameSampleIndex = frameIndex,
                .extent = kExtent,
                .fixedView = true,
                .accumulationResetToken = kResetToken,
                .denoiserResetToken = kResetToken,
                .hdrBuffer = &*hdrCV,
                .gNormalView = gNormal->view(),
                .gDepthView = gDepth->view(),
                .denoised = &*denoisedCV,
                .swapchainView = VK_NULL_HANDLE,
                .colorSpace = harmonia::utils::OutputColorSpace::eSDR,
            };
            passCV->record(cvContext);
            ASSERT_EQ(commandPool().endOneShot(*cmdCv), VK_SUCCESS);
        }
        const auto cvPixels = readRgbaImage(deviceCtx(), commandPool(), *denoisedCV);

        double cwSum = 0.0;
        double cvSum = 0.0;
        for (std::uint32_t y = 0; y < kExtent.height; ++y) {
            for (std::uint32_t x = 0; x < kExtent.width; ++x) {
                const std::size_t idx = static_cast<std::size_t>(y) * kExtent.width + x;
                const auto lw = 0.2627F * cwPixels[idx].r + 0.6780F * cwPixels[idx].g + 0.0593F * cwPixels[idx].b;
                const auto lv = 0.2627F * cvPixels[idx].r + 0.6780F * cvPixels[idx].g + 0.0593F * cvPixels[idx].b;
                cwSum += std::abs(static_cast<double>(lw) - static_cast<double>(kMu));
                cvSum += std::abs(static_cast<double>(lv) - static_cast<double>(kMu));
            }
        }
        const auto count = static_cast<double>(kExtent.width) * static_cast<double>(kExtent.height);
        const double cwErr = cwSum / count;
        const double cvErr = cvSum / count;
        harmonia::core::Logger::info(
            "DN3-M2 crossover: M={:.0f} constantW_err {:.5f} converging_err {:.5f}", sampleCount, cwErr, cvErr);
        if (first) {
            first = false;
        } else {
            EXPECT_LE(cvErr, prevCvErr + 1.0e-3) << "converging arm: error must not grow with M";
        }
        prevCvErr = cvErr;
        if (sampleCount == 4096.0F) {
            cwErr4096 = cwErr;
            cvErr4096 = cvErr;
        }
    }
    // The paper blend picks the better estimate per pixel: at the crossover it
    // must not be worse than the constant-W baseline beyond noise.
    EXPECT_LE(cvErr4096, cwErr4096 + 1.0e-3) << "paper blend must close the constant-W crossover lag";
}