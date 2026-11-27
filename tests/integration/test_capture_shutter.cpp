// Integration tests: the shutter of a sequence capture (--shutter with
// --time-samples).
//
// A capture renders every accumulation sample through the renderer, and a
// renderer reads the animation clock while it records. A stub renderer records
// those readings, so these tests observe the exact times a real capture hands
// to a renderer, sample by sample.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "fixtures/EmptySceneBuilder.hpp"
#include "harmonia/app/App.hpp"
#include "harmonia/core/Barrier.hpp"

namespace {

constexpr double kFps = 24.0;

class ShutterApp;

/// Clears the target and records the animation time of the sample.
class RecordingRenderer final : public harmonia::app::IRenderer {
  public:
    ShutterApp* app = nullptr;
    std::vector<double> sampleSeconds; ///< the clock at each record(), in order

    void record(VkCommandBuffer cmd, const harmonia::app::RenderTarget& target) noexcept override;
    void onResize(VkExtent2D) noexcept override {}
    const char* name() const noexcept override { return "ShutterRecordingProbe"; }
    VkPipelineStageFlags2 outputStageMask() const noexcept override { return VK_PIPELINE_STAGE_2_TRANSFER_BIT; }
    VkAccessFlags2 outputAccessMask() const noexcept override { return VK_ACCESS_2_TRANSFER_WRITE_BIT; }
};

class ShutterApp final : public harmonia::app::App {
  public:
    ShutterApp() { m_recording.app = this; }
    const RecordingRenderer& recording() const noexcept { return m_recording; }
    double sampleTime() const noexcept { return animationClock(kFps).seconds; }

  protected:
    bool onInitialize() override { return true; }
    bool onSceneLoaded(const harmonia::scene::SceneLoader::SceneConfig&) override { return true; }
    harmonia::app::IRenderer& renderer() noexcept override { return m_recording; }
    harmonia::scene::ISceneBuilder& sceneBuilder() noexcept override { return m_scene; }

  private:
    RecordingRenderer m_recording;
    EmptySceneBuilder m_scene;
};

void RecordingRenderer::record(VkCommandBuffer cmd, const harmonia::app::RenderTarget& target) noexcept {
    sampleSeconds.push_back(app->sampleTime());
    const auto barrier =
        harmonia::core::imageBarrier(target.image,
                                     sampleSeconds.size() == 1 ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL,
                                     VK_IMAGE_LAYOUT_GENERAL,
                                     VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                     VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                                     VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                     VK_ACCESS_2_TRANSFER_WRITE_BIT);
    harmonia::core::pipelineBarrier(cmd, std::span{&barrier, 1});
    const VkClearColorValue color{.float32 = {.25F, .25F, .25F, 1}};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, target.image, VK_IMAGE_LAYOUT_GENERAL, &color, 1, &range);
}

struct CaptureFiles {
    std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("harmonia_shutter_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    CaptureFiles() {
        std::filesystem::create_directory(root);
        std::ofstream(root / "empty.scene.toml")
            << "[render]\nmax_depth = 2\n"
               "\n[camera]\ntranslate = [0.0, 1.0, 4.0]\nrotate_x = -10.0\nrotate_y = 0.0\n";
    }
    ~CaptureFiles() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
    harmonia::app::App::Config config() const {
        harmonia::app::App::Config cfg;
        cfg.width = cfg.height = 8;
        cfg.sceneFile = root / "empty.scene.toml";
        cfg.assetsDir = root;
        cfg.outputFile = root / "out.exr";
        cfg.validation = true;
        cfg.noPostfx = true;
        cfg.stages = {.accumulation = true, .denoiser = false, .tonemapper = false};
        cfg.rngSeed = 7U;
        return cfg;
    }
    bool outputExists(std::uint32_t frame) const {
        const std::filesystem::path base = root / "out.exr";
        char suffix[16];
        std::snprintf(suffix, sizeof(suffix), "_%04u", frame);
        std::filesystem::path exr = base, png = base;
        exr.replace_filename("out" + std::string(suffix) + ".exr");
        png.replace_filename("out" + std::string(suffix) + ".png");
        return std::filesystem::exists(exr) && std::filesystem::exists(png);
    }
};

} // namespace

TEST(CaptureShutter, SamplesAreSpreadOverTheOpenShutterOfTheirOutputFrame) {
    CaptureFiles files;
    harmonia::app::App::Config cfg = files.config();
    cfg.sequenceFrames = 3U;
    cfg.offscreenFrames = 8U;
    cfg.shutter = 0.5F;
    cfg.timeSamples = 4U;
    constexpr double frameInterval = 1.0 / kFps;
    constexpr double open = 0.5 * frameInterval;
    const harmonia::app::App::Config reference = cfg; // the expected times come from the same settings
    ShutterApp app;

    ASSERT_EQ(app.run(std::move(cfg)), 0);

    const std::vector<double>& seconds = app.recording().sampleSeconds;
    ASSERT_EQ(seconds.size(), 3U * 8U) << "one record() per accumulation sample";
    for (std::uint32_t frame = 0U; frame < 3U; ++frame) {
        for (std::uint32_t sample = 0U; sample < 8U; ++sample) {
            const double t = seconds[frame * 8U + sample];
            // The accumulation frames of one time sample run consecutively: two per
            // instant here, the instants in their order.
            const std::uint32_t timeSample = harmonia::app::App::timeSampleOfFrame(sample, 8U, 4U);
            EXPECT_EQ(timeSample, sample / 2U) << "sample " << sample;
            // The sample's time is the clock's for exactly this output frame and
            // time sample, inside that sample's quarter of the frame's open shutter.
            EXPECT_EQ(t, harmonia::app::App::sequenceSampleClock(reference, kFps, frame, timeSample).seconds)
                << "frame " << frame << ", sample " << sample;
            EXPECT_GE(t, frame * frameInterval + open * static_cast<double>(timeSample) / 4.0);
            EXPECT_LT(t, frame * frameInterval + open * static_cast<double>(timeSample + 1U) / 4.0);
        }
        // The scene is asked for a new time once per instant, not once per accumulation frame.
        const std::set<double> instants(seconds.begin() + static_cast<std::ptrdiff_t>(frame) * 8,
                                        seconds.begin() + static_cast<std::ptrdiff_t>(frame + 1U) * 8);
        EXPECT_EQ(instants.size(), 4U) << "output frame " << frame;
        EXPECT_TRUE(files.outputExists(frame)) << "output frame " << frame;
    }
}

TEST(CaptureShutter, WithoutAShutterEverySampleRendersTheFrameInstant) {
    CaptureFiles files;
    harmonia::app::App::Config cfg = files.config();
    cfg.sequenceFrames = 2U;
    cfg.offscreenFrames = 3U;
    ShutterApp app;

    ASSERT_EQ(app.run(std::move(cfg)), 0);

    const std::vector<double>& seconds = app.recording().sampleSeconds;
    ASSERT_EQ(seconds.size(), 6U);
    for (std::size_t i = 0; i < seconds.size(); ++i) {
        EXPECT_DOUBLE_EQ(seconds[i], static_cast<double>(i / 3U) / kFps) << "sample " << i;
    }
}

TEST(CaptureShutter, RejectsAShutterOnAStill) {
    CaptureFiles files;
    harmonia::app::App::Config cfg = files.config();
    cfg.shutter = 0.5F;
    cfg.timeSamples = 4U;
    cfg.offscreenFrames = 8U;
    ShutterApp app;

    EXPECT_NE(app.run(std::move(cfg)), 0);
    EXPECT_TRUE(app.recording().sampleSeconds.empty()) << "rejected before any sample is rendered";
    EXPECT_FALSE(std::filesystem::exists(files.root / "out.exr"));
}

TEST(CaptureShutter, RejectsMoreTimeSamplesThanAccumulationSamples) {
    CaptureFiles files;
    harmonia::app::App::Config cfg = files.config();
    cfg.sequenceFrames = 2U;
    cfg.offscreenFrames = 2U;
    cfg.shutter = 0.5F;
    cfg.timeSamples = 4U;
    ShutterApp app;

    EXPECT_NE(app.run(std::move(cfg)), 0);
    EXPECT_TRUE(app.recording().sampleSeconds.empty());
    EXPECT_FALSE(files.outputExists(0U));
}
