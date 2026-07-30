// Component tests: fixed-view accumulation running average and reset behavior.

#include <volk/volk.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <gtest/gtest.h>
#include <slang-math/slang-math.hpp>

#include "fixtures/VulkanTestFixture.hpp"
#include "harmonia/core/Buffer.hpp"
#include "harmonia/core/Image.hpp"
#include "harmonia/pipeline/AccumulationPass.hpp"
#include "harmonia/pipeline/PassContext.hpp"

namespace {

[[nodiscard]] sm::float4
readFirstPixel(harmonia::CommandPool& commandPool, harmonia::Image& image, harmonia::Buffer& readback) {
    auto cmd = commandPool.beginOneShot();
    EXPECT_TRUE(cmd.has_value()) << static_cast<int>(cmd.error());
    if (!cmd) {
        return sm::float4(0.0F);
    }

    image.transition(*cmd,
                     VK_IMAGE_LAYOUT_GENERAL,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                     VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT |
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
    vkCmdCopyImageToBuffer(*cmd, image.handle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.handle(), 1, &region);
    image.transition(*cmd,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_IMAGE_LAYOUT_GENERAL,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                     VK_ACCESS_2_TRANSFER_READ_BIT,
                     VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                     VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT |
                         VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    EXPECT_EQ(commandPool.endOneShot(*cmd), VK_SUCCESS);

    const auto* pixels = static_cast<const sm::float4*>(readback.mappedData());
    return pixels[0];
}

} // namespace

TEST_F(VulkanFixture, AccumulationPass_ComputesRunningAverage) {
    constexpr VkExtent2D kExtent{8U, 8U};
    constexpr VkDeviceSize kReadbackBytes =
        static_cast<VkDeviceSize>(kExtent.width) * kExtent.height * sizeof(sm::float4);

    auto hdr = harmonia::Image::create(deviceCtx(),
                                       kExtent,
                                       VK_FORMAT_R32G32B32A32_SFLOAT,
                                       VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                           VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                       VK_IMAGE_ASPECT_COLOR_BIT,
                                       "test.accum.hdr");
    ASSERT_TRUE(hdr.has_value()) << static_cast<int>(hdr.error());

    auto readback = harmonia::Buffer::create(deviceCtx(),
                                             kReadbackBytes,
                                             VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                             VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                             "test.accum.readback");
    ASSERT_TRUE(readback.has_value()) << static_cast<int>(readback.error());
    ASSERT_NE(readback->mappedData(), nullptr);

    auto pass = harmonia::AccumulationPass::create(
        deviceCtx(), kExtent, std::filesystem::path(HARMONIA_SHADER_DIR) / "accumulation.spv");
    ASSERT_TRUE(pass.has_value()) << static_cast<int>(pass.error());
    pass->onResize(kExtent);

    const std::array colors{
        VkClearColorValue{.float32 = {0.2F, 0.4F, 0.6F, 1.0F}},
        VkClearColorValue{.float32 = {0.4F, 0.2F, 0.8F, 1.0F}},
        VkClearColorValue{.float32 = {0.8F, 0.8F, 0.2F, 1.0F}},
    };

    bool firstFrame = true;
    sm::float4 expected{};
    for (std::size_t frame = 0; frame < colors.size(); ++frame) {
        auto cmd = commandPool().beginOneShot();
        ASSERT_TRUE(cmd.has_value()) << static_cast<int>(cmd.error());
        hdr->transition(*cmd,
                        firstFrame ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        firstFrame ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                        firstFrame ? 0U
                                   : (VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT |
                                      VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT),
                        VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                        VK_ACCESS_2_TRANSFER_WRITE_BIT);
        firstFrame = false;

        const VkImageSubresourceRange fullRange{
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        };
        vkCmdClearColorImage(*cmd, hdr->handle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &colors[frame], 1, &fullRange);
        hdr->transition(*cmd,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_IMAGE_LAYOUT_GENERAL,
                        VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                        VK_ACCESS_2_TRANSFER_WRITE_BIT,
                        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                        VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT |
                            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

        const harmonia::PassContext passContext{
            .cmd = *cmd,
            .frameIndex = static_cast<std::uint32_t>(frame),
            .extent = kExtent,
            .fixedView = true,
            .accumulationResetToken = 1U,
            .hdrBuffer = &*hdr,
            .denoised = nullptr,
            .swapchainView = VK_NULL_HANDLE,
            .colorSpace = harmonia::OutputColorSpace::eSDR,
        };
        pass->record(passContext);
        ASSERT_EQ(commandPool().endOneShot(*cmd), VK_SUCCESS);

        const sm::float4 sample(
            colors[frame].float32[0], colors[frame].float32[1], colors[frame].float32[2], colors[frame].float32[3]);
        if (frame == 0) {
            expected = sample;
        } else {
            expected = expected + (sample - expected) * (1.0F / static_cast<float>(frame + 1));
        }

        const sm::float4 pixel = readFirstPixel(commandPool(), *hdr, *readback);
        EXPECT_NEAR(pixel.r, expected.r, 1e-4F) << "frame " << frame << " R";
        EXPECT_NEAR(pixel.g, expected.g, 1e-4F) << "frame " << frame << " G";
        EXPECT_NEAR(pixel.b, expected.b, 1e-4F) << "frame " << frame << " B";
        EXPECT_NEAR(pixel.a, expected.a, 1e-4F) << "frame " << frame << " A";
    }
}

TEST_F(VulkanFixture, AccumulationPass_ResetTokenInvalidatesHistory) {
    constexpr VkExtent2D kExtent{8U, 8U};
    constexpr VkDeviceSize kReadbackBytes =
        static_cast<VkDeviceSize>(kExtent.width) * kExtent.height * sizeof(sm::float4);

    auto hdr = harmonia::Image::create(deviceCtx(),
                                       kExtent,
                                       VK_FORMAT_R32G32B32A32_SFLOAT,
                                       VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                           VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                       VK_IMAGE_ASPECT_COLOR_BIT,
                                       "test.accum.reset.hdr");
    ASSERT_TRUE(hdr.has_value()) << static_cast<int>(hdr.error());

    auto readback = harmonia::Buffer::create(deviceCtx(),
                                             kReadbackBytes,
                                             VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                             VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                             "test.accum.reset.readback");
    ASSERT_TRUE(readback.has_value()) << static_cast<int>(readback.error());
    ASSERT_NE(readback->mappedData(), nullptr);

    auto pass = harmonia::AccumulationPass::create(
        deviceCtx(), kExtent, std::filesystem::path(HARMONIA_SHADER_DIR) / "accumulation.spv");
    ASSERT_TRUE(pass.has_value()) << static_cast<int>(pass.error());
    pass->onResize(kExtent);

    const std::array colors{
        VkClearColorValue{.float32 = {0.2F, 0.6F, 0.3F, 1.0F}},
        VkClearColorValue{.float32 = {0.6F, 0.2F, 0.9F, 1.0F}},
        VkClearColorValue{.float32 = {0.9F, 0.1F, 0.2F, 1.0F}},
    };
    const std::array<std::uint64_t, 3> resetTokens{1U, 1U, 2U};

    bool firstFrame = true;
    for (std::size_t frame = 0; frame < colors.size(); ++frame) {
        auto cmd = commandPool().beginOneShot();
        ASSERT_TRUE(cmd.has_value()) << static_cast<int>(cmd.error());
        hdr->transition(*cmd,
                        firstFrame ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        firstFrame ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                        firstFrame ? 0U
                                   : (VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT |
                                      VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT),
                        VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                        VK_ACCESS_2_TRANSFER_WRITE_BIT);
        firstFrame = false;

        const VkImageSubresourceRange fullRange{
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        };
        vkCmdClearColorImage(*cmd, hdr->handle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &colors[frame], 1, &fullRange);
        hdr->transition(*cmd,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_IMAGE_LAYOUT_GENERAL,
                        VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                        VK_ACCESS_2_TRANSFER_WRITE_BIT,
                        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                        VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT |
                            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

        const harmonia::PassContext passContext{
            .cmd = *cmd,
            .frameIndex = static_cast<std::uint32_t>(frame),
            .extent = kExtent,
            .fixedView = true,
            .accumulationResetToken = resetTokens[frame],
            .hdrBuffer = &*hdr,
            .denoised = nullptr,
            .swapchainView = VK_NULL_HANDLE,
            .colorSpace = harmonia::OutputColorSpace::eSDR,
        };
        pass->record(passContext);
        ASSERT_EQ(commandPool().endOneShot(*cmd), VK_SUCCESS);
    }

    const sm::float4 pixel = readFirstPixel(commandPool(), *hdr, *readback);
    const sm::float4 expected(colors[2].float32[0], colors[2].float32[1], colors[2].float32[2], colors[2].float32[3]);
    EXPECT_NEAR(pixel.r, expected.r, 1e-4F);
    EXPECT_NEAR(pixel.g, expected.g, 1e-4F);
    EXPECT_NEAR(pixel.b, expected.b, 1e-4F);
    EXPECT_NEAR(pixel.a, expected.a, 1e-4F);
}
