// DN3-M4 - production-scene convergence receipts (PLAN.md §5.2 DN3-M4 verification plan,
// recorded 2026-11-19 BEFORE this file existed).
//
// Receipt harness for the crossover fix on REAL content: it drives the shipped
// SceneOutputCopyPass chain (the same two-arm driver as
// SceneOutputCopyPass_DN3M2PaperBlendClosesCrossoverLag) over Theia-rendered raw
// accumulation ladders instead of the synthetic M2/M3 noise pattern:
//   * inputs : HARMONIA_DN3M4_DIR/<scene>_raw_<spp>.exr  (Theia `--output` captures =
//              raw accumulation, two-tier contract: denoiser forced off)
//   * outputs: HARMONIA_DN3M4_DIR/out/<scene>_{cw,cv}_<spp>.exr
//   * scenes : dragon_teapot (glass/transmission bias-floor regime) + cornell_classic
//              (diffuse-light control); spp = 16/64/256/1024/4096.
//
// Three modes per the plan: (1) raw = the input EXR itself (written through untouched by
// this test), (2) shipped constant-W = pass with shipped defaults (convergingMode false),
// (3) converging = shipped defaults + convergingMode. The ONLY difference between arms 2
// and 3 is the convergingMode knob - parameter-driven, no other setting diverges.
//
// Guides are the flat gNormal/gDepth contract from the M2/M3 test (the receipt measures
// the MODE difference under identical guides; a real-guide dump is a recorded follow-up).
// M is supplied per render by filename contract and uploaded as alpha, exactly like the
// M2/M3 test's `float4(value, ..., sampleCount)`.
//
// Curve metrics, acceptance and crops live in tools/dn3m4_receipt.py (the test only
// produces the mode EXRs and asserts structural validity), so the numeric gate stays in
// one tool alongside the other receipt scripts.
//
// Skips (GTEST_SKIP) unless HARMONIA_DN3M4_DIR points at a prepared ladder directory -
// the default suite must stay green without receipt inputs (RUN_SERIAL GPU test).

#include <volk/volk.h>

#include <OpenImageIO/imageio.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <gtest/gtest.h>
#include <limits>
#include <slang-math/slang-math.hpp>
#include <string>
#include <vector>

#include "fixtures/VulkanTestFixture.hpp"
#include "harmonia/core/Buffer.hpp"
#include "harmonia/core/Env.hpp"
#include "harmonia/core/Image.hpp"
#include "harmonia/core/Logger.hpp"
#include "harmonia/pipeline/PassContext.hpp"
#include "harmonia/pipeline/SceneOutputCopyPass.hpp"

namespace {

using harmonia::pipeline::SceneOutputCopyPassSettings;

// Ladder contract (must match tools/dn3m4_receipt.py).
constexpr std::array<const char*, 2> kDn3m4Scenes{"dragon_teapot", "cornell_classic"};
constexpr std::array<std::uint32_t, 5> kDn3m4Spp{16U, 64U, 256U, 1024U, 4096U};
constexpr std::uint64_t kDn3m4ResetToken = 11U; // constant across the ladder: the
                                                // shipped constant-W keeps history across
                                                // M (the bias-floor mechanism the receipt
                                                // must expose), mirroring the M2/M3 token.

// ── Local image helpers (same implementations as test_scene_output_copy_pass.cpp; that
//    file keeps its copies file-local, so this file does too) ────────────────────────

// Sibling-verbatim API contract (Buffer::create 5-arg + uploadData/mappedData, 7-arg
// Image::transition with explicit stage/access masks).

[[nodiscard]] std::vector<sm::float4> readRgbaImage(const harmonia::DeviceContext& deviceCtx,
                                                    harmonia::core::CommandPool& commandPool,
                                                    harmonia::core::Image& image) {
    const VkDeviceSize byteSize =
        static_cast<VkDeviceSize>(image.extent().width) * image.extent().height * sizeof(sm::float4);
    auto readback = harmonia::core::Buffer::create(deviceCtx,
                                                   byteSize,
                                                   VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                   VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                                   "test.dn3m4.readback");
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
                                                  "test.dn3m4.staging.rgba");
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
                                                  "test.dn3m4.staging.depth");
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

// ── EXR I/O (OIIO, raw_color like IblProbe::readEXR / ImageCapture::saveExr) ────────

struct LadderFrame {
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::vector<sm::float4> pixels; // alpha = M (filename contract)
};

[[nodiscard]] bool readLadderExr(const std::filesystem::path& path, std::uint32_t sampleCount, LadderFrame& out) {
    OIIO::ImageSpec openConfig;
    openConfig.attribute("raw_color", 1);
    const OIIO::ImageInput::unique_ptr inp = OIIO::ImageInput::open(path.string(), &openConfig);
    if (!inp) {
        harmonia::core::Logger::error("DN3-M4: cannot open '{}': {}", path.string(), OIIO::geterror());
        return false;
    }
    const OIIO::ImageSpec& spec = inp->spec();
    out.width = static_cast<std::uint32_t>(spec.width);
    out.height = static_cast<std::uint32_t>(spec.height);
    const std::size_t pixelCount = static_cast<std::size_t>(out.width) * out.height;
    const int channels = spec.nchannels;
    if (channels != 3 && channels != 4) {
        harmonia::core::Logger::error("DN3-M4: '{}' has {} channels (want 3 or 4)", path.string(), channels);
        return false;
    }
    std::vector<float> raw(pixelCount * static_cast<std::size_t>(channels));
    if (!inp->read_image(0, 0, 0, channels, OIIO::TypeDesc::FLOAT, raw.data())) {
        harmonia::core::Logger::error("DN3-M4: read failed for '{}': {}", path.string(), OIIO::geterror());
        return false;
    }
    out.pixels.resize(pixelCount);
    for (std::size_t i = 0; i < pixelCount; ++i) {
        const float* px = raw.data() + i * static_cast<std::size_t>(channels);
        out.pixels[i] = sm::float4{px[0], px[1], px[2], static_cast<float>(sampleCount)};
    }
    return true;
}

[[nodiscard]] bool writeModeExr(const std::filesystem::path& path,
                                std::uint32_t width,
                                std::uint32_t height,
                                const std::vector<sm::float4>& pixels) {
    const OIIO::ImageOutput::unique_ptr out = OIIO::ImageOutput::create(path.string());
    if (!out) {
        harmonia::core::Logger::error("DN3-M4: OIIO cannot create '{}': {}", path.string(), OIIO::geterror());
        return false;
    }
    OIIO::ImageSpec spec(static_cast<int>(width), static_cast<int>(height), 4, OIIO::TypeDesc::FLOAT);
    spec.attribute("raw_color", 1);
    if (!out->open(path.string(), spec)) {
        harmonia::core::Logger::error("DN3-M4: OIIO open failed for '{}': {}", path.string(), OIIO::geterror());
        return false;
    }
    std::vector<float> flat(pixels.size() * 4U);
    for (std::size_t i = 0; i < pixels.size(); ++i) {
        flat[i * 4U + 0U] = pixels[i].x;
        flat[i * 4U + 1U] = pixels[i].y;
        flat[i * 4U + 2U] = pixels[i].z;
        flat[i * 4U + 3U] = pixels[i].w;
    }
    if (!out->write_image(OIIO::TypeDesc::FLOAT, flat.data())) {
        harmonia::core::Logger::error("DN3-M4: OIIO write failed for '{}': {}", path.string(), OIIO::geterror());
        return false;
    }
    out->close();
    return true;
}

// One pass-chain arm state: shared across the ladder for one scene (constant-W and
// converging passes are created once per resolution, exactly like the M2/M3 test).

} // namespace

TEST_F(VulkanFixture, SceneOutputCopyPass_DN3M4ProductionSceneConvergence) {
    const auto dirRaw = harmonia::core::getEnvString("HARMONIA_DN3M4_DIR");
    if (!dirRaw) {
        GTEST_SKIP() << "set HARMONIA_DN3M4_DIR to a prepared DN3-M4 ladder directory to run this receipt";
    }
    const std::filesystem::path dir{*dirRaw};
    if (!std::filesystem::is_directory(dir)) {
        GTEST_SKIP() << "HARMONIA_DN3M4_DIR '" << dir.string() << "' does not exist";
    }
    const std::filesystem::path outDir = dir / "out";
    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);
    ASSERT_FALSE(ec) << ec.message();

    const std::filesystem::path denoiserSpv = std::filesystem::path(HARMONIA_SHADER_DIR) / "denoiser.spv";

    for (const char* scene : kDn3m4Scenes) {
        // Probe the first ladder rung for the resolution (real content drives the extent).
        const std::filesystem::path probePath = dir / (std::string(scene) + "_raw_16.exr");
        LadderFrame probe;
        ASSERT_TRUE(readLadderExr(probePath, 16U, probe)) << "missing ladder input " << probePath.string();
        const VkExtent2D extent{probe.width, probe.height};
        ASSERT_GT(extent.width, 0U);
        ASSERT_GT(extent.height, 0U);

        auto makeImage = [&](const char* name, VkFormat format) {
            return harmonia::core::Image::create(deviceCtx(),
                                                 extent,
                                                 format,
                                                 VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                                 VK_IMAGE_ASPECT_COLOR_BIT,
                                                 name);
        };
        auto hdrCW = makeImage("test.dn3m4.hdrCW", VK_FORMAT_R32G32B32A32_SFLOAT);
        auto hdrCV = makeImage("test.dn3m4.hdrCV", VK_FORMAT_R32G32B32A32_SFLOAT);
        auto denCW = makeImage("test.dn3m4.denCW", VK_FORMAT_R32G32B32A32_SFLOAT);
        auto denCV = makeImage("test.dn3m4.denCV", VK_FORMAT_R32G32B32A32_SFLOAT);
        auto gNormal = makeImage("test.dn3m4.gnormal", VK_FORMAT_R32G32B32A32_SFLOAT);
        auto gDepth = harmonia::core::Image::create(deviceCtx(),
                                                    extent,
                                                    VK_FORMAT_R32_SFLOAT,
                                                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                                    VK_IMAGE_ASPECT_COLOR_BIT,
                                                    "test.dn3m4.gdepth");
        ASSERT_TRUE(hdrCW && hdrCV && denCW && denCV && gNormal && gDepth);

        // Flat guides: the M2/M3 test contract (identical for both arms - the receipt
        // measures the mode difference under identical guides).
        const std::size_t pixelCount = static_cast<std::size_t>(extent.width) * extent.height;
        std::vector<sm::float4> normalPixels(pixelCount, sm::float4{0.5F, 0.5F, 1.0F, 0.0F});
        std::vector<float> depthPixels(pixelCount, 2.0F);
        uploadRgbaImage(deviceCtx(), commandPool(), *gNormal, normalPixels);
        uploadDepthImage(deviceCtx(), commandPool(), *gDepth, depthPixels);

        // Arm 1..2: shipped defaults; the converging arm differs ONLY by convergingMode.
        const SceneOutputCopyPassSettings shipped{};
        SceneOutputCopyPassSettings converging = shipped;
        converging.convergingMode = true;
        auto passCW = harmonia::pipeline::SceneOutputCopyPass::create(deviceCtx(), extent, denoiserSpv, shipped);
        auto passCV = harmonia::pipeline::SceneOutputCopyPass::create(deviceCtx(), extent, denoiserSpv, converging);
        ASSERT_TRUE(passCW.has_value() && passCV.has_value());
        passCW->onResize(extent);
        passCV->onResize(extent);

        for (const std::uint32_t sampleCount : kDn3m4Spp) {
            const std::filesystem::path inPath =
                dir / (std::string(scene) + "_raw_" + std::to_string(sampleCount) + ".exr");
            LadderFrame frame;
            ASSERT_TRUE(readLadderExr(inPath, sampleCount, frame)) << "missing ladder input " << inPath.string();
            ASSERT_EQ(frame.width, extent.width);
            ASSERT_EQ(frame.height, extent.height);
            uploadRgbaImage(deviceCtx(), commandPool(), *hdrCW, frame.pixels);
            uploadRgbaImage(deviceCtx(), commandPool(), *hdrCV, frame.pixels);

            // Constant-W arm: one record per M (shipped v0.7.9 blend, no chain).
            {
                auto cmd = commandPool().beginOneShot();
                ASSERT_TRUE(cmd.has_value());
                const harmonia::pipeline::PassContext ctx{
                    .cmd = *cmd,
                    .frameIndex = 0U,
                    .frameSampleIndex = sampleCount - 1U,
                    .extent = extent,
                    .fixedView = true,
                    .accumulationResetToken = kDn3m4ResetToken,
                    .denoiserResetToken = kDn3m4ResetToken,
                    .hdrBuffer = &*hdrCW,
                    .gNormalView = gNormal->view(),
                    .gDepthView = gDepth->view(),
                    .denoised = &*denCW,
                    .swapchainView = VK_NULL_HANDLE,
                    .colorSpace = harmonia::utils::OutputColorSpace::eSDR,
                };
                passCW->record(ctx);
                ASSERT_EQ(commandPool().endOneShot(*cmd), VK_SUCCESS);
                const auto pixels = readRgbaImage(deviceCtx(), commandPool(), *denCW);
                ASSERT_EQ(pixels.size(), pixelCount);
                for (const sm::float4& px : pixels) {
                    ASSERT_TRUE(std::isfinite(px.x) && std::isfinite(px.y) && std::isfinite(px.z))
                        << "constant-W output must be finite at M=" << sampleCount;
                }
                const std::filesystem::path outPath =
                    outDir / (std::string(scene) + "_cw_" + std::to_string(sampleCount) + ".exr");
                ASSERT_TRUE(writeModeExr(outPath, extent.width, extent.height, pixels)) << outPath.string();
            }

            // Converging arm: DISTINCT-STATE chain (real accumulation history).
            // Frame 0 carries the PREVIOUS ladder rung (its own image + alpha = its
            // sample count), frames 1-2 the current rung - so the Eq. 7/SURE weight
            // sees genuine temporal variance. With identical frames the weight sees
            // zero variance and selects raw at every rung (recorded in PLAN).
            LadderFrame prevFrame;
            bool hasPrev = false;
            const auto sIt = std::find(kDn3m4Spp.begin(), kDn3m4Spp.end(), sampleCount);
            if (sIt != kDn3m4Spp.end() && sIt != kDn3m4Spp.begin()) {
                const std::uint32_t prevSpp = *(sIt - 1);
                const std::filesystem::path prevPath =
                    dir / (std::string(scene) + "_raw_" + std::to_string(prevSpp) + ".exr");
                hasPrev = readLadderExr(prevPath, prevSpp, prevFrame);
                ASSERT_TRUE(hasPrev) << "missing previous rung " << prevPath.string();
            }
            {
                std::vector<sm::float4> pixels;
                const std::vector<sm::float4>& currentPixels = frame.pixels; // outer LadderFrame
                for (std::uint32_t frame = 0U; frame < 3U; ++frame) {
                    uploadRgbaImage(deviceCtx(),
                                    commandPool(),
                                    *hdrCV,
                                    (frame == 0U && hasPrev) ? prevFrame.pixels : currentPixels);
                    auto cmd = commandPool().beginOneShot();
                    ASSERT_TRUE(cmd.has_value());
                    const harmonia::pipeline::PassContext ctx{
                        .cmd = *cmd,
                        .frameIndex = frame,
                        .frameSampleIndex = sampleCount - 1U + frame, // mirrors DN3-M2's chain (M-1, M, M+1)
                        .extent = extent,
                        .fixedView = true,
                        .accumulationResetToken = kDn3m4ResetToken,
                        .denoiserResetToken = kDn3m4ResetToken,
                        .hdrBuffer = &*hdrCV,
                        .gNormalView = gNormal->view(),
                        .gDepthView = gDepth->view(),
                        .denoised = &*denCV,
                        .swapchainView = VK_NULL_HANDLE,
                        .colorSpace = harmonia::utils::OutputColorSpace::eSDR,
                    };
                    passCV->record(ctx);
                    ASSERT_EQ(commandPool().endOneShot(*cmd), VK_SUCCESS);
                }
                pixels = readRgbaImage(deviceCtx(), commandPool(), *denCV);
                ASSERT_EQ(pixels.size(), pixelCount);
                for (const sm::float4& px : pixels) {
                    ASSERT_TRUE(std::isfinite(px.x) && std::isfinite(px.y) && std::isfinite(px.z))
                        << "converging output must be finite at M=" << sampleCount;
                }
                const std::filesystem::path outPath =
                    outDir / (std::string(scene) + "_cv_" + std::to_string(sampleCount) + ".exr");
                ASSERT_TRUE(writeModeExr(outPath, extent.width, extent.height, pixels)) << outPath.string();
            }

            harmonia::core::Logger::info("DN3-M4: {} M={} modes written to {}", scene, sampleCount, outDir.string());
        }
    }
}
