// Component tests: the lens glare stage against the reference in fixtures/GlareReference.hpp.

#include <volk/volk.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <gtest/gtest.h>
#include <limits>
#include <random>
#include <slang-math/slang-math.hpp>
#include <string>
#include <vector>

#include "fixtures/GlareReference.hpp"
#include "fixtures/VulkanTestFixture.hpp"
#include "harmonia/core/Buffer.hpp"
#include "harmonia/core/Image.hpp"
#include "harmonia/pipeline/GlarePass.hpp"
#include "harmonia/pipeline/PassContext.hpp"

namespace {

namespace reference = glare_reference;
using harmonia::core::Image;
using harmonia::pipeline::GlarePass;

/// Every frame of these tests is scene-referred RGBA, 32-bit float.
constexpr VkFormat kFrameFormat = VK_FORMAT_R32G32B32A32_SFLOAT;

[[nodiscard]] std::vector<sm::float4> toGpu(const reference::Plane& plane) {
    std::vector<sm::float4> pixels;
    pixels.reserve(plane.size());
    for (const reference::Pixel& pixel : plane) {
        pixels.emplace_back(
            static_cast<float>(pixel[0]), static_cast<float>(pixel[1]), static_cast<float>(pixel[2]), 1.0F);
    }
    return pixels;
}

[[nodiscard]] std::expected<Image, VkResult>
makeImage(const harmonia::DeviceContext& ctx, VkExtent2D extent, const char* name) {
    return Image::create(ctx,
                         extent,
                         kFrameFormat,
                         VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                         VK_IMAGE_ASPECT_COLOR_BIT,
                         name);
}

void upload(harmonia::core::CommandPool& pool, Image& image, const std::vector<sm::float4>& pixels) {
    const VkDeviceSize byteSize =
        static_cast<VkDeviceSize>(image.extent().width) * image.extent().height * sizeof(sm::float4);
    auto staging = harmonia::core::Buffer::create(g_vulkanTestCtx->deviceCtx(),
                                                  byteSize,
                                                  VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                                  VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                                  "test.glare.staging");
    ASSERT_TRUE(staging.has_value()) << static_cast<int>(staging.error());
    staging->uploadData(pixels.data(), byteSize);

    auto cmd = pool.beginOneShot();
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
                     VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    ASSERT_EQ(pool.endOneShot(*cmd), VK_SUCCESS);
}

/// The glare texture of the last recorded frame. It comes back where it was, in
/// SHADER_READ_ONLY_OPTIMAL.
[[nodiscard]] reference::Plane readGlare(harmonia::core::CommandPool& pool, const Image& glare) {
    const VkExtent2D extent = glare.extent();
    const VkDeviceSize byteSize = static_cast<VkDeviceSize>(extent.width) * extent.height * sizeof(sm::float4);
    auto readback = harmonia::core::Buffer::create(g_vulkanTestCtx->deviceCtx(),
                                                   byteSize,
                                                   VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                   VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                                   "test.glare.readback");
    EXPECT_TRUE(readback.has_value()) << static_cast<int>(readback.error());
    if (!readback) {
        return {};
    }
    auto cmd = pool.beginOneShot();
    EXPECT_TRUE(cmd.has_value()) << static_cast<int>(cmd.error());
    if (!cmd) {
        return {};
    }
    glare.transition(*cmd,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                     VK_ACCESS_2_SHADER_READ_BIT,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                     VK_ACCESS_2_TRANSFER_READ_BIT);
    const VkBufferImageCopy region{
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .imageOffset = {0, 0, 0},
        .imageExtent = {extent.width, extent.height, 1},
    };
    vkCmdCopyImageToBuffer(*cmd, glare.handle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback->handle(), 1, &region);
    glare.transition(*cmd,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                     VK_ACCESS_2_TRANSFER_READ_BIT,
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                     VK_ACCESS_2_SHADER_READ_BIT);
    EXPECT_EQ(pool.endOneShot(*cmd), VK_SUCCESS);

    reference::Plane plane(static_cast<std::size_t>(extent.width) * extent.height);
    const auto* pixels = static_cast<const sm::float4*>(readback->mappedData());
    for (std::size_t index = 0; index < plane.size(); ++index) {
        plane[index] = {pixels[index].r, pixels[index].g, pixels[index].b};
    }
    return plane;
}

/// The GPU adds in float32, in an order of its own: every sum of non-negative terms
/// picks up at most one rounding (2^-24 of what it sums) per term, and a level sums
/// up to 52 terms. Over the seven sums of the longest chain that is 2.1e-5 of the
/// scale the reference names for each texel; the bound allows twice that.
constexpr double kRounding = 5.0e-5;

void expectMatches(const reference::Plane& got, const reference::Plane& expected, const reference::Plane& errorScale) {
    ASSERT_EQ(got.size(), expected.size());
    ASSERT_EQ(got.size(), errorScale.size());
    std::size_t outside = 0U;
    double worst = 0.0; ///< the largest error, as a share of its tolerance
    std::string first;
    for (std::size_t index = 0; index < got.size(); ++index) {
        for (std::size_t component = 0; component < 3; ++component) {
            const double tolerance =
                1.0e-6 * std::abs(expected[index][component]) + kRounding * errorScale[index][component];
            const double error = std::abs(got[index][component] - expected[index][component]);
            worst = std::max(worst, error / tolerance);
            if (!(error <= tolerance) && outside++ == 0U) {
                first = "the first one: texel " + std::to_string(index) + ", component " + std::to_string(component) +
                        ": got " + std::to_string(got[index][component]) + ", expected " +
                        std::to_string(expected[index][component]) + ", tolerance " + std::to_string(tolerance);
            }
        }
    }
    EXPECT_EQ(outside, 0U) << first << " (the largest error is " << worst << " of its tolerance)";
}

[[nodiscard]] VkExtent2D extentOf(reference::Size size) {
    return VkExtent2D{size.width, size.height};
}

/// The frame of the reference test: a spread of light around the threshold, single
/// bright texels far above it, a negative component, and texels that are not light.
[[nodiscard]] reference::Plane mixedFrame(reference::Size size, std::uint32_t seed) {
    std::mt19937 engine{seed};
    std::uniform_real_distribution<double> spread(0.0, 2.5);
    reference::Plane plane(reference::count(size));
    for (reference::Pixel& pixel : plane) {
        pixel = {spread(engine), spread(engine), spread(engine)};
    }
    const auto put = [&plane, size](std::uint32_t x, std::uint32_t y, reference::Pixel value) {
        plane[static_cast<std::size_t>(y) * size.width + x] = value;
    };
    put(7U, 5U, {2000.0, 1800.0, 1600.0});
    put(size.width / 2U, size.height / 3U, {1.0e6, 1.0e6, 1.0e6});
    put(size.width - 5U, size.height - 2U, {-5.0, 0.3, 0.2});
    put(2U, size.height - 2U, {std::numeric_limits<double>::infinity(), 1.0, 1.0});
    put(size.width - 1U, 0U, {std::numeric_limits<double>::quiet_NaN(), 1.0, 4.0});
    put(3U, 3U, {0.9, 0.9, 0.9});
    return plane;
}

[[nodiscard]] std::filesystem::path shaderDirectory() {
    return std::filesystem::path(HARMONIA_SHADER_DIR);
}

} // namespace

TEST_F(VulkanFixture, GlarePass_MatchesTheReference) {
    constexpr std::uint32_t kSeed = 20261126U;
    auto glare = GlarePass::create(deviceCtx(),
                                   VkExtent2D{45U, 30U},
                                   shaderDirectory(),
                                   GlarePass::Config{.threshold = 1.0F, .levelWeight = 0.8F});
    ASSERT_TRUE(glare.has_value()) << static_cast<int>(glare.error());
    EXPECT_EQ(glare->levelCount(), 4U) << "23x15, 12x8, 6x4, 3x2";

    // The frame, through the record() that names the writers of the frame.
    {
        const reference::Size kSize{45U, 30U};
        const reference::Plane frame = mixedFrame(kSize, kSeed);
        auto source = makeImage(deviceCtx(), VkExtent2D{45U, 30U}, "test.glare.frame");
        ASSERT_TRUE(source.has_value()) << static_cast<int>(source.error());
        upload(commandPool(), *source, toGpu(frame));

        auto cmd = commandPool().beginOneShot();
        ASSERT_TRUE(cmd.has_value()) << static_cast<int>(cmd.error());
        glare->record(*cmd, *source, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        ASSERT_EQ(commandPool().endOneShot(*cmd), VK_SUCCESS);

        const reference::Plane got = readGlare(commandPool(), glare->glareImage());
        const reference::Result expected = reference::glareOf(frame, kSize, 1.0, 0.8);
        EXPECT_EQ(glare->glareImage().extent().width, expected.size.width);
        EXPECT_EQ(glare->glareImage().extent().height, expected.size.height);
        expectMatches(got, expected.glare, expected.errorScale);
    }

    // A resized frame through the PassContext form: the pass makes its levels anew.
    {
        const reference::Size kSize{20U, 16U};
        const reference::Plane frame = mixedFrame(kSize, kSeed + 1U);
        glare->onResize(VkExtent2D{20U, 16U});
        EXPECT_EQ(glare->levelCount(), 3U) << "10x8, 5x4, 3x2";

        auto source = makeImage(deviceCtx(), VkExtent2D{20U, 16U}, "test.glare.small");
        ASSERT_TRUE(source.has_value()) << static_cast<int>(source.error());
        upload(commandPool(), *source, toGpu(frame));

        auto cmd = commandPool().beginOneShot();
        ASSERT_TRUE(cmd.has_value()) << static_cast<int>(cmd.error());
        const harmonia::pipeline::PassContext context{
            .cmd = *cmd,
            .extent = VkExtent2D{20U, 16U},
            .hdrBuffer = &*source,
        };
        glare->record(context);
        ASSERT_EQ(commandPool().endOneShot(*cmd), VK_SUCCESS);

        const reference::Plane got = readGlare(commandPool(), glare->glareImage());
        const reference::Result expected = reference::glareOf(frame, kSize, 1.0, 0.8);
        expectMatches(got, expected.glare, expected.errorScale);
    }
}

TEST_F(VulkanFixture, GlarePass_GivesAConstantItsExcessAtEveryWidth) {
    // One colour above the threshold everywhere: every downsampled level keeps it,
    // at the border too, and the upsampled chain adds 1 + w + w^2 + ... of it.
    const reference::Size kSize{32U, 32U};
    const double kValue = 3.0;
    reference::Plane frame(reference::count(kSize), reference::Pixel{kValue, kValue, kValue});
    auto source = makeImage(deviceCtx(), extentOf(kSize), "test.glare.constant");
    ASSERT_TRUE(source.has_value()) << static_cast<int>(source.error());
    upload(commandPool(), *source, toGpu(frame));

    constexpr float kWeight = 0.8F;
    auto glare = GlarePass::create(
        deviceCtx(), extentOf(kSize), shaderDirectory(), GlarePass::Config{.threshold = 1.0F, .levelWeight = kWeight});
    ASSERT_TRUE(glare.has_value()) << static_cast<int>(glare.error());
    ASSERT_EQ(glare->levelCount(), 4U) << "16x16, 8x8, 4x4, 2x2";

    auto cmd = commandPool().beginOneShot();
    ASSERT_TRUE(cmd.has_value()) << static_cast<int>(cmd.error());
    glare->record(*cmd, *source, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    ASSERT_EQ(commandPool().endOneShot(*cmd), VK_SUCCESS);

    // The excess of the colour above the threshold is 2, to within the rounding of
    // the luminance weights (1e-15), and the series is 1 + 0.8 + 0.64 + 0.512 for a
    // weight of exactly 0.8. The pass takes the weight as a float, and so does the
    // series here.
    double series = 0.0;
    double power = 1.0;
    for (std::uint32_t level = 0; level < glare->levelCount(); ++level) {
        series += power;
        power *= kWeight;
    }
    ASSERT_NEAR(series / 2.952, 1.0, 1.0e-7);
    const double expected = (kValue - 1.0) * series;

    const reference::Plane got = readGlare(commandPool(), glare->glareImage());
    ASSERT_EQ(got.size(), reference::count(reference::halfOf(kSize)));
    for (std::size_t index = 0; index < got.size(); ++index) {
        for (std::size_t component = 0; component < 3; ++component) {
            EXPECT_NEAR(got[index][component], expected, 1.0e-5 * expected) << "texel " << index;
        }
    }
}

TEST_F(VulkanFixture, GlarePass_ScattersNothingBelowTheThreshold) {
    // A frame whose luminance lies below the knee: nothing is scattered, exactly.
    const reference::Size kSize{24U, 16U};
    std::mt19937 engine{7U};
    std::uniform_real_distribution<double> dark(0.0, 0.4);
    reference::Plane frame(reference::count(kSize));
    for (reference::Pixel& pixel : frame) {
        pixel = {dark(engine), dark(engine), dark(engine)};
    }
    auto source = makeImage(deviceCtx(), extentOf(kSize), "test.glare.dark");
    ASSERT_TRUE(source.has_value()) << static_cast<int>(source.error());
    upload(commandPool(), *source, toGpu(frame));

    auto glare = GlarePass::create(deviceCtx(), extentOf(kSize), shaderDirectory());
    ASSERT_TRUE(glare.has_value()) << static_cast<int>(glare.error());

    auto cmd = commandPool().beginOneShot();
    ASSERT_TRUE(cmd.has_value()) << static_cast<int>(cmd.error());
    glare->record(*cmd, *source, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    ASSERT_EQ(commandPool().endOneShot(*cmd), VK_SUCCESS);

    const reference::Plane got = readGlare(commandPool(), glare->glareImage());
    ASSERT_FALSE(got.empty());
    for (const reference::Pixel& pixel : got) {
        for (double component : pixel) {
            EXPECT_DOUBLE_EQ(component, 0.0);
        }
    }
}

TEST_F(VulkanFixture, GlarePass_LevelsFollowTheExtent) {
    struct Case {
        VkExtent2D extent;
        std::uint32_t levels;
        const char* sizes;
    };
    const Case cases[]{
        {VkExtent2D{4U, 2U}, 0U, "no level: the half is 2x1"},
        {VkExtent2D{3U, 3U}, 1U, "2x2"},
        {VkExtent2D{7U, 7U}, 2U, "4x4, 2x2"},
        {VkExtent2D{45U, 30U}, 4U, "23x15, 12x8, 6x4, 3x2"},
        {VkExtent2D{64U, 64U}, 5U, "32x32 down to 2x2"},
        {VkExtent2D{1024U, 1024U}, 6U, "the most levels there are"},
    };
    auto glare = GlarePass::create(deviceCtx(), cases[0].extent, shaderDirectory());
    ASSERT_TRUE(glare.has_value()) << static_cast<int>(glare.error());
    for (const Case& item : cases) {
        SCOPED_TRACE(std::to_string(item.extent.width) + "x" + std::to_string(item.extent.height) + ": " + item.sizes);
        glare->onResize(item.extent);
        EXPECT_EQ(glare->levelCount(), item.levels);
        EXPECT_EQ(glare->isValid(), item.levels > 0U);
    }
}

TEST_F(VulkanFixture, GlarePass_RejectsAConfigurationItCouldNotRun) {
    for (const float threshold : {0.0F, -1.0F}) {
        auto glare = GlarePass::create(
            deviceCtx(), VkExtent2D{32U, 32U}, shaderDirectory(), GlarePass::Config{.threshold = threshold});
        EXPECT_FALSE(glare.has_value()) << "threshold " << threshold;
    }
    for (const float weight : {-0.1F, 1.1F}) {
        auto glare = GlarePass::create(deviceCtx(),
                                       VkExtent2D{32U, 32U},
                                       shaderDirectory(),
                                       GlarePass::Config{.threshold = 1.0F, .levelWeight = weight});
        EXPECT_FALSE(glare.has_value()) << "level weight " << weight;
    }
}
