// Component test: the scene-output copy pass should preserve pixels exactly.

#include <volk/volk.h>

#include <array>
#include <glm/glm.hpp>
#include <gtest/gtest.h>

#include "fixtures/VulkanTestFixture.hpp"
#include "harmonia/core/Buffer.hpp"
#include "harmonia/core/Image.hpp"
#include "harmonia/pipeline/PassContext.hpp"
#include "harmonia/pipeline/SceneOutputCopyPass.hpp"

TEST_F(VulkanFixture, SceneOutputCopyPass_PreservesHdrPixels) {
    constexpr VkExtent2D kExtent{8U, 8U};
    constexpr VkClearColorValue kClear{.float32 = {0.25F, 0.5F, 0.75F, 1.0F}};
    constexpr VkDeviceSize kReadbackBytes = static_cast<VkDeviceSize>(kExtent.width) * kExtent.height * sizeof(glm::vec4);

    auto hdr = Image::create(deviceCtx(),
                             kExtent,
                             VK_FORMAT_R32G32B32A32_SFLOAT,
                             VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                             VK_IMAGE_ASPECT_COLOR_BIT,
                             "test.sceneoutput.hdr");
    ASSERT_TRUE(hdr.has_value()) << static_cast<int>(hdr.error());

    auto denoised = Image::create(deviceCtx(),
                                  kExtent,
                                  VK_FORMAT_R32G32B32A32_SFLOAT,
                                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                  VK_IMAGE_ASPECT_COLOR_BIT,
                                  "test.sceneoutput.denoised");
    ASSERT_TRUE(denoised.has_value()) << static_cast<int>(denoised.error());

    auto readback = Buffer::create(deviceCtx(),
                                   kReadbackBytes,
                                   VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                   VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                   "test.sceneoutput.readback");
    ASSERT_TRUE(readback.has_value()) << static_cast<int>(readback.error());
    ASSERT_NE(readback->mappedData(), nullptr);

    harmonia::SceneOutputCopyPass pass;
    pass.onResize(kExtent);

    auto cmd = commandPool().beginOneShot();
    ASSERT_TRUE(cmd.has_value()) << static_cast<int>(cmd.error());

    hdr->transition(*cmd,
                    VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                    0,
                    VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT);
    const VkImageSubresourceRange fullRange{
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = 1,
    };
    vkCmdClearColorImage(*cmd, hdr->handle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &kClear, 1, &fullRange);
    hdr->transition(*cmd,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_GENERAL,
                    VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                    VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

    PassContext passContext{
        .cmd = *cmd,
        .frameIndex = 0,
        .extent = kExtent,
        .hdrBuffer = &*hdr,
        .gNormal = nullptr,
        .gDepth = nullptr,
        .denoised = &*denoised,
        .swapchainView = VK_NULL_HANDLE,
        .colorSpace = OutputColorSpace::eSDR,
    };
    pass.record(passContext);

    denoised->transition(*cmd,
                         VK_IMAGE_LAYOUT_GENERAL,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                         VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                         VK_ACCESS_2_TRANSFER_READ_BIT);
    const VkBufferImageCopy region{
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .imageOffset = {0, 0, 0},
        .imageExtent = {kExtent.width, kExtent.height, 1},
    };
    vkCmdCopyImageToBuffer(*cmd, denoised->handle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback->handle(), 1, &region);

    ASSERT_EQ(commandPool().endOneShot(*cmd), VK_SUCCESS);

    const auto* pixels = static_cast<const glm::vec4*>(readback->mappedData());
    const glm::vec4 expected(kClear.float32[0], kClear.float32[1], kClear.float32[2], kClear.float32[3]);
    for (uint32_t i = 0; i < kExtent.width * kExtent.height; ++i) {
        EXPECT_NEAR(pixels[i].r, expected.r, 1e-5F) << "pixel " << i << " R";
        EXPECT_NEAR(pixels[i].g, expected.g, 1e-5F) << "pixel " << i << " G";
        EXPECT_NEAR(pixels[i].b, expected.b, 1e-5F) << "pixel " << i << " B";
        EXPECT_NEAR(pixels[i].a, expected.a, 1e-5F) << "pixel " << i << " A";
    }
}
