#include <chrono>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <span>

#include "fixtures/VulkanFaultScope.hpp"
#include "harmonia/app/App.hpp"
#include "harmonia/core/Barrier.hpp"

namespace {
class EmptyScene final : public harmonia::scene::ISceneBuilder {
  public:
    std::uint32_t addMaterial(harmonia::scene::Material&&) override { return 0; }
    std::uint32_t addTexture(harmonia::scene::Texture&&) override { return 0; }
    std::uint32_t addMesh(const harmonia::DeviceContext&,
                          const harmonia::core::CommandPool&,
                          harmonia::scene::MeshData&&,
                          const harmonia::scene::MeshOpacity&,
                          std::string_view) override {
        return 0;
    }
    std::uint32_t addSphereMesh(const harmonia::DeviceContext&,
                                const harmonia::core::CommandPool&,
                                float,
                                std::string_view) override {
        return 0;
    }
    std::uint32_t addInstance(std::uint32_t, const harmonia::scene::Transform&, std::uint32_t) override { return 0; }
    const aether::OpacityMicromapData& addOpacityMicromap(aether::OpacityMicromapData&&) override { return m_omm; }

  private:
    aether::OpacityMicromapData m_omm;
};

class ClearRenderer final : public harmonia::app::IRenderer {
  public:
    VulkanFaultScope* submissionFault = nullptr;
    VulkanFaultScope* slotWaitFault = nullptr;
    VkDevice device = VK_NULL_HANDLE;
    std::uint32_t frames = 0;
    void record(VkCommandBuffer cmd, const harmonia::app::RenderTarget& target) noexcept override {
        const auto barrier =
            harmonia::core::imageBarrier(target.image,
                                         frames == 0 ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL,
                                         VK_IMAGE_LAYOUT_GENERAL,
                                         VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                         VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                                         VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                         VK_ACCESS_2_TRANSFER_WRITE_BIT);
        harmonia::core::pipelineBarrier(cmd, std::span{&barrier, 1});
        const VkClearColorValue color{.float32 = {.25F, .25F, .25F, 1}};
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, target.image, VK_IMAGE_LAYOUT_GENERAL, &color, 1, &range);
        if (++frames == 2) {
            auto* fault = submissionFault ? submissionFault : slotWaitFault;
            if (fault) {
                fault->deviceForReset = device;
                fault->arm();
            }
        }
    }
    void onResize(VkExtent2D) noexcept override { frames = 0; }
    const char* name() const noexcept override { return "ClearFailureProbe"; }
    VkPipelineStageFlags2 outputStageMask() const noexcept override { return VK_PIPELINE_STAGE_2_TRANSFER_BIT; }
    VkAccessFlags2 outputAccessMask() const noexcept override { return VK_ACCESS_2_TRANSFER_WRITE_BIT; }
};

class FailureApp final : public harmonia::app::App {
  public:
    ClearRenderer clear;
    VulkanFaultScope* stageFault = nullptr;
    VulkanFaultScope* initializeFault = nullptr;
    VulkanFaultScope* resizeFault = nullptr;
    std::uint32_t resized = 0;
    std::uint32_t updateCalls = 0;
    std::uint32_t resizedAtFault = 0, updatesAtFault = 0;
    bool queueSecondResize = false;
    ~FailureApp() override {
        if (deviceContext().device != VK_NULL_HANDLE)
            VulkanFaultScope::cleanupForTest(deviceContext().device);
    }
    std::uint32_t completedFrames() const { return frameIndex(); }

  protected:
    bool onInitialize() override {
        clear.device = deviceContext().device;
        if (initializeFault)
            initializeFault->arm();
        return true;
    }
    harmonia::app::IRenderer& renderer() noexcept override { return clear; }
    harmonia::scene::ISceneBuilder& sceneBuilder() noexcept override { return scene; }
    bool onSceneLoaded(const harmonia::scene::SceneLoader::SceneConfig&) override {
        if (stageFault)
            stageFault->arm();
        if (resizeFault) {
            resizeFault->deviceForReset = deviceContext().device;
            SDL_Event event{};
            event.type = SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED;
            event.window.windowID = SDL_GetWindowID(window());
            event.window.data1 = 41773;
            event.window.data2 = 8;
            EXPECT_TRUE(SDL_PushEvent(&event));
            if (queueSecondResize) {
                EXPECT_TRUE(SDL_PushEvent(&event));
            }
        }
        return true;
    }
    bool onEvent(const SDL_Event& event) override {
        if (resizeFault && event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED && event.window.data1 == 41773) {
            if (!faultObserved) {
                resizedAtFault = resized;
                updatesAtFault = updateCalls;
                faultObserved = true;
            }
            resizeFault->arm();
        }
        return false;
    }
    void onResized(VkExtent2D) override { ++resized; }
    void onUpdate(float) override {
        ++updateCalls;
        if (resizeFault && ++updates > 2)
            requestQuit();
    }

  private:
    EmptyScene scene;
    std::uint32_t updates = 0;
    bool faultObserved = false;
};

struct CaptureFiles {
    std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("harmonia_failure_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    CaptureFiles() {
        std::filesystem::create_directory(root);
        std::ofstream(root / "empty.scene.toml") << "[render]\nmax_depth = 2\n";
    }
    ~CaptureFiles() {
        std::error_code ec;
        for (const auto* name : {"empty.scene.toml", "out.exr", "out.png"})
            std::filesystem::remove(root / name, ec);
        std::filesystem::remove(root, ec);
    }
    harmonia::app::App::Config config() const {
        harmonia::app::App::Config cfg;
        cfg.width = cfg.height = 8;
        cfg.sceneFile = root / "empty.scene.toml";
        cfg.assetsDir = root;
        cfg.outputFile = root / "out.exr";
        cfg.offscreenFrames = 3;
        cfg.validation = true;
        cfg.noPostfx = true;
        cfg.stages = {.accumulation = true, .denoiser = false, .tonemapper = false};
        return cfg;
    }
    void expectNoImages() const {
        EXPECT_FALSE(std::filesystem::exists(root / "out.exr"));
        EXPECT_FALSE(std::filesystem::exists(root / "out.png"));
    }
};
} // namespace

TEST(CaptureFailureContract, RequestedAccumulationFailureDoesNotExport) {
    CaptureFiles files;
    VulkanFaultScope faults;
    faults.failComputeOnce = true;
    FailureApp app;
    app.stageFault = &faults;
    EXPECT_NE(app.run(files.config()), 0);
    EXPECT_TRUE(faults.computeInjected);
    EXPECT_EQ(app.clear.frames, 0U);
    files.expectNoImages();
}

TEST(CaptureFailureContract, LaterSubmitFailureRetiresEarlierWorkAndDoesNotExport) {
    CaptureFiles files;
    VulkanFaultScope faults;
    faults.failSubmitOnce = true;
    faults.idleAllocationFailures = 1;
    FailureApp app;
    app.clear.submissionFault = &faults;
    EXPECT_NE(app.run(files.config()), 0);
    EXPECT_TRUE(faults.submitInjected);
    EXPECT_EQ(app.completedFrames(), 1U);
    EXPECT_GE(faults.idleCalls, 2U);
    EXPECT_TRUE(faults.retiredBySubject);
    EXPECT_FALSE(faults.unsafeResourceDestruction);
    EXPECT_FALSE(faults.unsafeReset);
    files.expectNoImages();
}

TEST(CaptureFailureContract, RecycledSlotWaitFailureDoesNotResetPendingCommands) {
    CaptureFiles files;
    VulkanFaultScope faults;
    faults.failWaitOnce = true;
    faults.idleAllocationFailures = 1;
    FailureApp app;
    app.clear.slotWaitFault = &faults;
    EXPECT_NE(app.run(files.config()), 0);
    EXPECT_TRUE(faults.waitInjected);
    EXPECT_EQ(app.completedFrames(), 2U);
    EXPECT_EQ(app.clear.frames, 2U);
    EXPECT_TRUE(faults.retiredBySubject);
    EXPECT_FALSE(faults.unsafeReset);
    EXPECT_FALSE(faults.unsafeResourceDestruction);
    files.expectNoImages();
}

TEST(CaptureFailureContract, ShutdownRetiresBeforeDestroyingResources) {
    CaptureFiles files;
    VulkanFaultScope faults;
    {
        FailureApp app;
        ASSERT_EQ(app.run(files.config()), 0);
        faults.idleAllocationFailures = 1;
        faults.arm();
    }
    EXPECT_TRUE(faults.idleInjected);
    EXPECT_TRUE(faults.retiredBySubject);
    EXPECT_FALSE(faults.unsafeResourceDestruction);
    EXPECT_FALSE(faults.unsafeReset);
}

TEST(CaptureFailureContract, ResizeRetiresBeforeResetAndResourceReplacement) {
    CaptureFiles files;
    VulkanFaultScope faults;
    faults.idleAllocationFailures = 1;
    FailureApp app;
    app.resizeFault = &faults;
    auto cfg = files.config();
    cfg.outputFile.clear();
    EXPECT_EQ(app.run(std::move(cfg)), 0);
    EXPECT_GE(app.resized, 1U);
    EXPECT_TRUE(faults.idleInjected);
    EXPECT_TRUE(faults.retiredBySubject);
    EXPECT_FALSE(faults.unsafeReset);
    EXPECT_FALSE(faults.unsafeResourceDestruction);
}

TEST(CaptureFailureContract, SceneDisabledAccumulationDoesNotCreateAnUnusedStage) {
    CaptureFiles files;
    std::ofstream(files.root / "empty.scene.toml") << "[render]\nenable_accumulation_stage = false\n";
    VulkanFaultScope faults;
    faults.failComputeOnce = true;
    FailureApp app;
    app.initializeFault = &faults;
    auto cfg = files.config();
    cfg.offscreenFrames = 1;
    EXPECT_EQ(app.run(std::move(cfg)), 0);
    EXPECT_FALSE(faults.computeInjected);
    EXPECT_TRUE(std::filesystem::exists(files.root / "out.exr"));
}

TEST(CaptureFailureContract, FatalRecreateStopsQueuedEventsAndClientUpdates) {
    CaptureFiles files;
    VulkanFaultScope faults;
    faults.failSwapchain = true;
    FailureApp app;
    app.resizeFault = &faults;
    app.queueSecondResize = true;
    auto cfg = files.config();
    cfg.outputFile.clear();
    EXPECT_NE(app.run(std::move(cfg)), 0);
    EXPECT_TRUE(faults.swapchainInjected);
    EXPECT_EQ(faults.swapchainCalls, 1U);
    EXPECT_EQ(app.updateCalls, app.updatesAtFault);
    EXPECT_EQ(app.resized, app.resizedAtFault);
}
