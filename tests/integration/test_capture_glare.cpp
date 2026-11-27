// Integration tests: the lens glare of a capture.
//
// A capture writes the scene-referred EXR before the glare runs, and the tone
// mapper writes the PNG with the glare. A stub renderer copies a known frame
// into the HDR image, and the files are compared with references in double
// precision:
//
//   - the EXR is the frame, without glare;
//   - the PNG is the display path of tonemap.slang: the share of the frame's
//     glare (fixtures/GlareReference.hpp, read under bilinear filtering) added
//     to the frame, the tone curve, the sRGB encoding, 8 bits.
//
// The frames keep the tone mapper's firefly softener quiet (no texel is more
// than four times as bright as its brightest neighbour), so that it takes no
// part in the display path.

#include <volk/volk.h>

#include <OpenImageIO/imageio.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <random>
#include <slang-math/slang-math.hpp>
#include <string>
#include <vector>

#include "fixtures/EmptySceneBuilder.hpp"
#include "fixtures/GlareReference.hpp"
#include "harmonia/app/App.hpp"
#include "harmonia/core/Barrier.hpp"
#include "harmonia/core/Buffer.hpp"

namespace {

namespace reference = glare_reference;
using Vec3 = std::array<double, 3>;

constexpr reference::Size kSize{40U, 24U}; ///< four levels: 20x12, 10x6, 5x3, 3x2
constexpr float kStrength = 0.1F;

// ---------------------------------------------------------------------------
// Frames: RGBA float, the precision the GPU receives
// ---------------------------------------------------------------------------

using Frame = std::vector<sm::float4>;

[[nodiscard]] reference::Plane toReference(const Frame& frame) {
    reference::Plane plane;
    plane.reserve(frame.size());
    for (const sm::float4& pixel : frame) {
        plane.push_back(reference::Pixel{pixel.r, pixel.g, pixel.b});
    }
    return plane;
}

/// A frame around and above the threshold: a spread of light, a block of it far
/// above, and one with colour. Every texel is at most four times as bright as its
/// brightest neighbour, so the tone mapper's firefly softener leaves it alone.
[[nodiscard]] Frame litFrame() {
    std::mt19937 engine{20261126U};
    std::uniform_real_distribution<float> spread(0.0F, 1.5F);
    Frame frame(reference::count(kSize));
    for (sm::float4& pixel : frame) {
        pixel = sm::float4(spread(engine), spread(engine), spread(engine), 1.0F);
    }
    const auto put = [&frame](std::uint32_t x, std::uint32_t y, sm::float3 value) {
        frame[static_cast<std::size_t>(y) * kSize.width + x] = sm::float4(value, 1.0F);
    };
    for (std::uint32_t y = 8U; y <= 10U; ++y) {
        for (std::uint32_t x = 10U; x <= 12U; ++x) {
            put(x, y, sm::float3(30.0F, 30.0F, 30.0F));
        }
    }
    for (std::uint32_t y = 15U; y <= 18U; ++y) {
        for (std::uint32_t x = 28U; x <= 32U; ++x) {
            put(x, y, sm::float3(8.0F, 12.0F, 16.0F));
        }
    }
    return frame;
}

/// A frame whose luminance lies below the knee of the threshold.
[[nodiscard]] Frame darkFrame() {
    std::mt19937 engine{7U};
    std::uniform_real_distribution<float> dark(0.0F, 0.4F);
    Frame frame(reference::count(kSize));
    for (sm::float4& pixel : frame) {
        pixel = sm::float4(dark(engine), dark(engine), dark(engine), 1.0F);
    }
    return frame;
}

[[nodiscard]] double luminanceOf(const reference::Pixel& pixel) {
    return 0.2627 * pixel[0] + 0.6780 * pixel[1] + 0.0593 * pixel[2];
}

/// Whether the softener is quiet for a frame: the tone mapper scales a texel down
/// when its luminance is above four times that of its brightest neighbour.
[[nodiscard]] bool softenerIsQuiet(const reference::Plane& frame) {
    for (std::uint32_t y = 0U; y < kSize.height; ++y) {
        for (std::uint32_t x = 0U; x < kSize.width; ++x) {
            double brightest = 0.0;
            for (std::int32_t dy = -1; dy <= 1; ++dy) {
                for (std::int32_t dx = -1; dx <= 1; ++dx) {
                    const auto column = static_cast<std::uint32_t>(
                        std::clamp(static_cast<std::int32_t>(x) + dx, 0, static_cast<std::int32_t>(kSize.width) - 1));
                    const auto row = static_cast<std::uint32_t>(
                        std::clamp(static_cast<std::int32_t>(y) + dy, 0, static_cast<std::int32_t>(kSize.height) - 1));
                    brightest =
                        std::max(brightest, luminanceOf(frame[static_cast<std::size_t>(row) * kSize.width + column]));
                }
            }
            if (luminanceOf(frame[static_cast<std::size_t>(y) * kSize.width + x]) > 4.0 * brightest) {
                return false;
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// The display path of the capture, in double precision
// ---------------------------------------------------------------------------

/// Rec.2020 -> Rec.709.
[[nodiscard]] Vec3 toRec709(const Vec3& color) {
    return {1.6605 * color[0] - 0.5876 * color[1] - 0.0728 * color[2],
            -0.1246 * color[0] + 1.1329 * color[1] - 0.0083 * color[2],
            -0.0182 * color[0] - 0.1006 * color[1] + 1.1187 * color[2]};
}

enum class ToneCurve {
    kAces,
    kReinhard,
};

/// The SDR tone curves of the capture: Rec.2020 linear in, Rec.709 linear out.
[[nodiscard]] Vec3 toneCurve(const Vec3& color, ToneCurve curve) {
    if (curve == ToneCurve::kReinhard) {
        // Luminance through x / (1 + x), the chromaticity kept.
        const Vec3 rec709 = toRec709(color);
        const double luminance = 0.2627 * rec709[0] + 0.6780 * rec709[1] + 0.0593 * rec709[2];
        if (luminance <= 0.0) {
            return Vec3{0.0, 0.0, 0.0};
        }
        return Vec3{rec709[0] / (1.0 + luminance), rec709[1] / (1.0 + luminance), rec709[2] / (1.0 + luminance)};
    }
    // ACES: Rec.2020 -> AP1, the RRT+ODT fit, AP1 -> Rec.709.
    constexpr double kToAp1[3][3]{
        {0.6131324, 0.3395255, 0.0474491},
        {0.0701243, 0.9163394, 0.0135363},
        {0.0205076, 0.1096098, 0.8699926},
    };
    constexpr double kToRec709[3][3]{
        {2.9090, -1.6933, -0.2159},
        {-0.3595, 1.3712, -0.0114},
        {-0.0447, -0.2477, 1.2925},
    };
    Vec3 fit{};
    Vec3 out{};
    for (std::size_t row = 0; row < 3; ++row) {
        const double ap1 =
            std::max(kToAp1[row][0] * color[0] + kToAp1[row][1] * color[1] + kToAp1[row][2] * color[2], 0.0);
        const double numerator = ap1 * (ap1 + 0.0245786) - 0.000090537;
        const double denominator = ap1 * (0.983729 * ap1 + 0.4329510) + 0.238081;
        fit[row] = numerator / denominator;
    }
    for (std::size_t row = 0; row < 3; ++row) {
        out[row] = std::max(kToRec709[row][0] * fit[0] + kToRec709[row][1] * fit[1] + kToRec709[row][2] * fit[2], 0.0);
    }
    return out;
}

/// The sRGB encoding of a display-referred value.
[[nodiscard]] double srgb(double value) {
    value = std::max(value, 0.0);
    return value <= 0.0031308 ? 12.92 * value : 1.055 * std::pow(value, 1.0 / 2.4) - 0.055;
}

/// The byte the capture writes for a display-referred value: clamped, encoded, 8 bits.
[[nodiscard]] int displayByte(double value) {
    return static_cast<int>(std::lround(255.0 * std::clamp(srgb(value), 0.0, 1.0)));
}

// ---------------------------------------------------------------------------
// A capture of a frame
// ---------------------------------------------------------------------------

/// The files of one capture: the PNG (RGB, 8 bits) and the EXR (RGB float).
struct Capture {
    std::vector<std::uint8_t> png;
    reference::Size pngSize{};
    std::vector<float> exr;
};

class CaptureFiles {
  public:
    explicit CaptureFiles(ToneCurve curve)
        : root(std::filesystem::temp_directory_path() /
               ("harmonia_capture_glare_" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
        std::filesystem::create_directory(root);
        std::ofstream(root / "empty.scene.toml")
            << "[render]\nmax_depth = 2\n"
               "\n[camera]\ntranslate = [0.0, 1.0, 4.0]\nrotate_x = -10.0\nrotate_y = 0.0\n"
               "\n[tonemap]\ntonemapper = \""
            << (curve == ToneCurve::kAces ? "aces" : "reinhard") << "\"\n";
    }
    ~CaptureFiles() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
    CaptureFiles(const CaptureFiles&) = delete;
    CaptureFiles& operator=(const CaptureFiles&) = delete;

    [[nodiscard]] harmonia::app::App::Config config(float glare, bool noPostfx) const {
        harmonia::app::App::Config cfg;
        cfg.width = kSize.width;
        cfg.height = kSize.height;
        cfg.sceneFile = root / "empty.scene.toml";
        cfg.assetsDir = root;
        cfg.outputFile = root / "out.exr";
        cfg.noPostfx = noPostfx;
        cfg.glare = glare;
        cfg.offscreenFrames = 1U;
        cfg.stages = {.accumulation = false, .denoiser = false, .tonemapper = true};
        return cfg;
    }

    const std::filesystem::path root;
};

/// Copies a known frame into the HDR image whenever it records.
class FrameRenderer final : public harmonia::app::IRenderer {
  public:
    [[nodiscard]] bool setSource(const harmonia::DeviceContext& ctx, const Frame& pixels) {
        const VkDeviceSize byteSize = pixels.size() * sizeof(sm::float4);
        auto staging = harmonia::core::Buffer::create(
            ctx, byteSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_HOST, "test.glare.frame");
        if (!staging) {
            return false;
        }
        staging->uploadData(pixels.data(), byteSize);
        m_staging = std::move(*staging);
        return true;
    }

    void record(VkCommandBuffer cmd, const harmonia::app::RenderTarget& target) noexcept override {
        const bool first = m_copies++ == 0U;
        const std::array toCopy{
            harmonia::core::imageBarrier(target.image,
                                         first ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL,
                                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                         first ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                         first ? 0U : (VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT),
                                         VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                         VK_ACCESS_2_TRANSFER_WRITE_BIT)};
        harmonia::core::pipelineBarrier(cmd, toCopy);
        const VkBufferImageCopy region{
            .bufferOffset = 0,
            .bufferRowLength = 0,
            .bufferImageHeight = 0,
            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .imageOffset = {0, 0, 0},
            .imageExtent = {target.extent.width, target.extent.height, 1},
        };
        vkCmdCopyBufferToImage(cmd, m_staging.handle(), target.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        const std::array copied{
            harmonia::core::imageBarrier(target.image,
                                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                         VK_IMAGE_LAYOUT_GENERAL,
                                         VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                         VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                         VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                         VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT)};
        harmonia::core::pipelineBarrier(cmd, copied);
    }

    void onResize(VkExtent2D) noexcept override {}
    [[nodiscard]] const char* name() const noexcept override { return "FrameProbe"; }
    [[nodiscard]] VkPipelineStageFlags2 outputStageMask() const noexcept override {
        return VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    }
    [[nodiscard]] VkAccessFlags2 outputAccessMask() const noexcept override { return VK_ACCESS_2_TRANSFER_WRITE_BIT; }

  private:
    harmonia::core::Buffer m_staging;
    std::uint32_t m_copies = 0U;
};

class FrameApp final : public harmonia::app::App {
  public:
    explicit FrameApp(Frame frame) : m_pixels(std::move(frame)) {}

  protected:
    bool onInitialize() override { return m_renderer.setSource(deviceContext(), m_pixels); }
    bool onSceneLoaded(const harmonia::scene::SceneLoader::SceneConfig&) override { return true; }
    [[nodiscard]] harmonia::app::IRenderer& renderer() noexcept override { return m_renderer; }
    [[nodiscard]] harmonia::scene::ISceneBuilder& sceneBuilder() noexcept override { return m_scene; }

  private:
    const Frame m_pixels;
    FrameRenderer m_renderer;
    EmptySceneBuilder m_scene;
};

[[nodiscard]] Capture capture(ToneCurve curve, const Frame& frame, float glare, bool noPostfx = false) {
    CaptureFiles files(curve);
    FrameApp app(frame);
    Capture result;
    if (app.run(files.config(glare, noPostfx)) != 0) {
        ADD_FAILURE() << "the capture did not succeed";
        return result;
    }
    auto png = OIIO::ImageInput::open((files.root / "out.png").string());
    if (png) {
        const OIIO::ImageSpec& spec = png->spec();
        result.pngSize = {static_cast<std::uint32_t>(spec.width), static_cast<std::uint32_t>(spec.height)};
        result.png.resize(static_cast<std::size_t>(spec.width) * spec.height * 3U);
        if (spec.nchannels != 3 || !png->read_image(0, 0, 0, 3, OIIO::TypeDesc::UINT8, result.png.data())) {
            result.png.clear();
        }
        png->close();
    }
    auto exr = OIIO::ImageInput::open((files.root / "out.exr").string());
    if (exr) {
        const OIIO::ImageSpec& spec = exr->spec();
        result.exr.resize(static_cast<std::size_t>(spec.width) * spec.height * 3U);
        if (!exr->read_image(0, 0, 0, 3, OIIO::TypeDesc::FLOAT, result.exr.data())) {
            result.exr.clear();
        }
        exr->close();
    }
    return result;
}

} // namespace

// ---------------------------------------------------------------------------
// With the two tone curves of the reference
// ---------------------------------------------------------------------------

class CaptureGlare : public ::testing::TestWithParam<ToneCurve> {
  public:
    [[nodiscard]] static std::string name(const ::testing::TestParamInfo<ToneCurve>& info) {
        return info.param == ToneCurve::kAces ? "Aces" : "Reinhard";
    }
};

TEST_P(CaptureGlare, PngIsTheToneCurveOfTheFrameWithItsGlare) {
    const Frame frame = litFrame();
    const reference::Plane referenceFrame = toReference(frame);
    ASSERT_TRUE(softenerIsQuiet(referenceFrame));

    const Capture got = capture(GetParam(), frame, kStrength);
    ASSERT_EQ(got.pngSize, kSize);
    ASSERT_EQ(got.png.size(), reference::count(kSize) * 3U);

    const reference::Result glare = reference::glareOf(referenceFrame, kSize, 1.0, 0.8);
    ASSERT_EQ(glare.levelCount, 4U);
    std::size_t outside = 0U;
    int worst = 0;
    for (std::uint32_t y = 0U; y < kSize.height; ++y) {
        for (std::uint32_t x = 0U; x < kSize.width; ++x) {
            const std::size_t index = static_cast<std::size_t>(y) * kSize.width + x;
            const reference::Pixel light = reference::glareAt(glare, kSize, x, y);
            Vec3 value{};
            for (std::size_t component = 0; component < 3; ++component) {
                value[component] = referenceFrame[index][component] + kStrength * light[component];
            }
            const Vec3 display = toneCurve(value, GetParam());
            for (std::size_t component = 0; component < 3; ++component) {
                // 8 bits. The device rounds its own float32 result to a byte, and
                // measurably not to the nearest one: on the reference GPU a value
                // flips to the next byte about 1/32 to 2/32 of a step above the
                // boundary. So one step of difference is the device's, not a fault.
                const int difference =
                    std::abs(static_cast<int>(got.png[3 * index + component]) - displayByte(display[component]));
                worst = std::max(worst, difference);
                if (difference > 1) {
                    ++outside;
                }
            }
        }
    }
    EXPECT_EQ(outside, 0U) << "the largest difference is " << worst << " of 255";
}

TEST_P(CaptureGlare, ExrIsTheFrameWithoutGlare) {
    const Frame frame = litFrame();

    const Capture got = capture(GetParam(), frame, kStrength);
    ASSERT_EQ(got.exr.size(), reference::count(kSize) * 3U);

    for (std::size_t index = 0; index < got.exr.size(); index += 3) {
        const sm::float4& pixel = frame[index / 3];
        EXPECT_FLOAT_EQ(got.exr[index], pixel.r) << "texel " << index / 3 << " R";
        EXPECT_FLOAT_EQ(got.exr[index + 1], pixel.g) << "texel " << index / 3 << " G";
        EXPECT_FLOAT_EQ(got.exr[index + 2], pixel.b) << "texel " << index / 3 << " B";
    }
}

INSTANTIATE_TEST_SUITE_P(ToneCurves,
                         CaptureGlare,
                         ::testing::Values(ToneCurve::kAces, ToneCurve::kReinhard),
                         CaptureGlare::name);

// ---------------------------------------------------------------------------
// Whatever the tone curve
// ---------------------------------------------------------------------------

TEST(CaptureGlareStages, NothingIsScatteredBelowTheThreshold) {
    const Frame frame = darkFrame();

    const Capture plain = capture(ToneCurve::kAces, frame, 0.0F);
    const Capture glare = capture(ToneCurve::kAces, frame, 0.5F);
    ASSERT_EQ(plain.png.size(), reference::count(kSize) * 3U);
    ASSERT_EQ(glare.png.size(), plain.png.size());

    EXPECT_EQ(glare.png, plain.png) << "the two PNGs are the same bytes";
}

TEST(CaptureGlareStages, NoPostfxTurnsTheGlareOff) {
    const Frame frame = litFrame();
    const reference::Plane referenceFrame = toReference(frame);
    ASSERT_TRUE(softenerIsQuiet(referenceFrame));

    const Capture plain = capture(ToneCurve::kAces, frame, 0.0F);
    const Capture off = capture(ToneCurve::kAces, frame, 0.5F, /*noPostfx=*/true);
    ASSERT_EQ(plain.png.size(), reference::count(kSize) * 3U);
    ASSERT_EQ(off.png.size(), plain.png.size());

    EXPECT_EQ(off.png, plain.png) << "the two PNGs are the same bytes";
}
