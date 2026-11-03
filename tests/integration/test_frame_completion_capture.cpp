// Real App capture/accumulation contract; synthetic producer status is GPU-written.
#include <OpenImageIO/imagebuf.h>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "fixtures/VulkanFaultScope.hpp"
#include "harmonia/app/App.hpp"
#include "harmonia/app/IFrameCompletion.hpp"
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

class DelayedRenderer final : public harmonia::app::IRenderer, public harmonia::app::IFrameCompletion {
  public:
    ~DelayedRenderer() override {
        if (m_status != VK_NULL_HANDLE)
            vmaDestroyBuffer(m_allocator, m_status, m_allocation);
    }
    bool create(const harmonia::DeviceContext& ctx) {
        m_allocator = ctx.allocator;
        VkBufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size = 4;
        info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo allocation{};
        allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        allocation.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
        VmaAllocationInfo mapped{};
        if (vmaCreateBuffer(m_allocator, &info, &allocation, &m_status, &m_allocation, &mapped) != VK_SUCCESS)
            return false;
        m_mapped = mapped.pMappedData;
        return m_mapped != nullptr;
    }
    void record(VkCommandBuffer cmd, const harmonia::app::RenderTarget& target) noexcept override {
        EXPECT_EQ(chunks, 0U);
        m_target = target.image;
        clear(cmd, providerEnabled ? 0.0F : (logicalFrames == 0 ? 0.25F : 0.75F));
        vkCmdFillBuffer(cmd, m_status, 0, 4, 0);
        hostBarrier(cmd);
        ++logicalFrames;
        if (logicalFrames == 2U && secondFrameFault != nullptr)
            secondFrameFault->arm();
    }
    harmonia::app::IFrameCompletion* frameCompletion() noexcept override { return providerEnabled ? this : nullptr; }
    std::expected<bool, VkResult> ready() noexcept override {
        if (fail)
            return std::unexpected(VK_ERROR_UNKNOWN);
        const auto result = vmaInvalidateAllocation(m_allocator, m_allocation, 0, VK_WHOLE_SIZE);
        if (result != VK_SUCCESS)
            return std::unexpected(result);
        return *static_cast<const std::uint32_t*>(m_mapped) == 1U;
    }
    VkResult recordContinuation(VkCommandBuffer cmd) noexcept override {
        ++chunks;
        ++totalChunks;
        if (chunks == 3U) {
            clear(cmd, logicalFrames == 1U ? 0.25F : 0.75F);
            vkCmdFillBuffer(cmd, m_status, 0, 4, 1);
        }
        hostBarrier(cmd);
        return VK_SUCCESS;
    }
    VkPipelineStageFlags2 outputStageMask() const noexcept override { return VK_PIPELINE_STAGE_2_TRANSFER_BIT; }
    VkAccessFlags2 outputAccessMask() const noexcept override { return VK_ACCESS_2_TRANSFER_WRITE_BIT; }
    void onResize(VkExtent2D) noexcept override { m_firstUse = true; }
    const char* name() const noexcept override { return "DelayedCompletionProbe"; }
    std::uint32_t logicalFrames = 0, chunks = 0, totalChunks = 0;
    bool fail = false;
    bool providerEnabled = true;
    VulkanFaultScope* secondFrameFault = nullptr;

  private:
    static void hostBarrier(VkCommandBuffer cmd) {
        const VkMemoryBarrier2 memory{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                      .pNext = nullptr,
                                      .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                      .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
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
        vkCmdPipelineBarrier2(cmd, &dependency);
    }
    void clear(VkCommandBuffer cmd, float value) {
        const auto barrier = harmonia::core::imageBarrier(
            m_target,
            m_firstUse ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL,
            VK_IMAGE_LAYOUT_GENERAL,
            m_firstUse ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            m_firstUse ? 0U
                       : (VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT),
            VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT);
        harmonia::core::pipelineBarrier(cmd, std::span{&barrier, 1});
        m_firstUse = false;
        const VkClearColorValue color{.float32 = {value, value, value, 1}};
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, m_target, VK_IMAGE_LAYOUT_GENERAL, &color, 1, &range);
    }
    VmaAllocator m_allocator{};
    VkBuffer m_status{};
    VmaAllocation m_allocation{};
    void* m_mapped{};
    VkImage m_target{};
    bool m_firstUse = true;
};

class CompletionApp final : public harmonia::app::App {
  public:
    ~CompletionApp() override {
        // Test safety net only. Fault tests assert retirement BEFORE run returns,
        // so this destructor cannot mask missing host-loop retirement.
        if (deviceContext().device != VK_NULL_HANDLE)
            VulkanFaultScope::cleanupForTest(deviceContext().device);
    }
    DelayedRenderer delayed;
    std::vector<std::uint32_t> frameIds;
    VulkanFaultScope* stageCreationFault = nullptr;
    VulkanFaultScope* resizeCreationFault = nullptr;
    std::uint32_t completedSecondResize = 0;
    std::uint32_t completedLogicalFrames() const noexcept { return frameIndex(); }

  protected:
    harmonia::app::IRenderer& renderer() noexcept override { return delayed; }
    harmonia::scene::ISceneBuilder& sceneBuilder() noexcept override { return m_scene; }
    bool onInitialize() override { return delayed.create(deviceContext()); }
    bool onSceneLoaded(const harmonia::scene::SceneLoader::SceneConfig&) override {
        if (stageCreationFault != nullptr)
            stageCreationFault->arm();
        if (resizeCreationFault != nullptr) {
            SDL_Event first{};
            first.type = SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED;
            first.window.windowID = SDL_GetWindowID(window());
            first.window.data1 = 41773;
            first.window.data2 = 8;
            SDL_Event second = first;
            second.window.data1 = 41774;
            EXPECT_TRUE(SDL_PushEvent(&first));
            EXPECT_TRUE(SDL_PushEvent(&second));
        }
        return true;
    }
    bool onEvent(const SDL_Event& event) override {
        if (resizeCreationFault != nullptr && event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
            if (event.window.data1 == 41773) {
                m_resizeMarker = 1;
                resizeCreationFault->arm();
            } else if (event.window.data1 == 41774)
                m_resizeMarker = 2;
        }
        return false;
    }
    void onResized(VkExtent2D) override {
        if (m_resizeMarker == 2)
            ++completedSecondResize;
        m_resizeMarker = 0;
    }
    void onUpdate(float) override {
        // Test safety net: a failed synthetic event setup must not leave a GUI
        // loop running forever. Assertions below still reject that execution.
        if (resizeCreationFault != nullptr && ++m_updates > 2)
            requestQuit();
    }
    void onSceneFrameReady(VkCommandBuffer) noexcept override {
        if (delayed.providerEnabled) {
            const auto ready = delayed.ready();
            EXPECT_TRUE(ready.has_value() && *ready);
            EXPECT_EQ(delayed.chunks, 3U);
        }
        frameIds.push_back(frameIndex());
        delayed.chunks = 0;
    }

  private:
    EmptyScene m_scene;
    int m_resizeMarker = 0;
    std::uint32_t m_updates = 0;
};

class CaptureFiles {
  public:
    CaptureFiles() {
        const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
        root = std::filesystem::temp_directory_path() / ("harmonia_completion_" + std::to_string(suffix));
        if (!std::filesystem::create_directory(root))
            return;
        scene = root / "empty.scene.toml";
        output = root / "capture.exr";
        std::ofstream file(scene);
        file << "[render]\nworking_color_space = \"lin_rec2020_scene\"\n"
                "enable_denoiser_stage = false\nenable_tonemapper_stage = false\n";
    }
    ~CaptureFiles() {
        std::error_code ec;
        for (const auto& file : {scene, output, root / "capture.png"})
            std::filesystem::remove(file, ec);
        std::filesystem::remove(root, ec); // nonrecursive: never delete unexpected files
    }
    harmonia::app::AppConfig config() const {
        harmonia::app::AppConfig cfg;
        cfg.width = cfg.height = 8;
        cfg.sceneFile = scene;
        cfg.assetsDir = root;
        cfg.outputFile = output;
        cfg.offscreenFrames = 2;
        cfg.noPostfx = true;
        cfg.stages = {.accumulation = true, .denoiser = false, .tonemapper = false};
        return cfg;
    }
    std::filesystem::path root, scene, output;
};

} // namespace

TEST(FrameCompletionCapture, WaitsBeforeAccumulationAndPreservesLogicalFrameIds) {
    CaptureFiles files;
    CompletionApp app;
    ASSERT_EQ(app.run(files.config()), 0);
    EXPECT_EQ(app.delayed.logicalFrames, 2U);
    EXPECT_EQ(app.delayed.totalChunks, 6U);
    EXPECT_EQ(app.frameIds, (std::vector<std::uint32_t>{0, 1}));
    ASSERT_TRUE(std::filesystem::exists(files.output));
    OIIO::ImageBuf image(files.output.string());
    ASSERT_TRUE(image.read());
    float pixel[4]{};
    for (int y = 0; y < image.spec().height; ++y) {
        for (int x = 0; x < image.spec().width; ++x) {
            image.getpixel(x, y, pixel, 4);
            for (int channel = 0; channel < 3; ++channel)
                EXPECT_NEAR(pixel[channel], 0.5F, 1.0e-6F) << x << "," << y;
        }
    }
}

TEST(FrameCompletionCapture, FailureDoesNotExportAnIncompleteImage) {
    CaptureFiles files;
    CompletionApp app;
    app.delayed.fail = true;
    EXPECT_NE(app.run(files.config()), 0);
    EXPECT_EQ(app.delayed.logicalFrames, 1U);
    EXPECT_TRUE(app.frameIds.empty());
    EXPECT_FALSE(std::filesystem::exists(files.output));
    EXPECT_FALSE(std::filesystem::exists(files.root / "capture.png"));
}

TEST(FrameCompletionCapture, LaterSubmitFailureRetiresBeforeReturningToRendererOwner) {
    CaptureFiles files;
    VulkanFaultScope faults;
    faults.failSubmitOnce = true;
    faults.idleAllocationFailures = 1;
    CompletionApp app;
    app.delayed.providerEnabled = false;
    app.delayed.secondFrameFault = &faults;
    EXPECT_NE(app.run(files.config()), 0);
    EXPECT_TRUE(faults.submitInjected);
    EXPECT_EQ(app.completedLogicalFrames(), 1U);
    EXPECT_GE(faults.idleCalls, 2U);
    EXPECT_TRUE(faults.retiredBySubject) << "App must retire earlier submissions before derived renderer teardown";
    EXPECT_FALSE(std::filesystem::exists(files.output));
}

TEST(FrameCompletionCapture, FailedProducerWaitRetiresBeforeDestroyingItsSemaphore) {
    CaptureFiles files;
    VulkanFaultScope faults;
    faults.failWaitOnce = true;
    faults.idleAllocationFailures = 1;
    CompletionApp app;
    app.delayed.secondFrameFault = &faults;
    EXPECT_NE(app.run(files.config()), 0);
    EXPECT_TRUE(faults.waitInjected);
    EXPECT_EQ(app.completedLogicalFrames(), 1U);
    EXPECT_TRUE(faults.retiredBySubject);
    EXPECT_FALSE(faults.unsafeSemaphoreDestruction);
    EXPECT_FALSE(faults.unsafeCommandFree);
    EXPECT_FALSE(faults.testRecoveryUsed);
    EXPECT_FALSE(std::filesystem::exists(files.output));
}

TEST(FrameCompletionCapture, RequestedAccumulationCreationFailureAbortsCapture) {
    CaptureFiles files;
    VulkanFaultScope faults;
    faults.failComputeOnce = true;
    CompletionApp app;
    app.stageCreationFault = &faults;
    EXPECT_NE(app.run(files.config()), 0);
    EXPECT_TRUE(faults.computeInjected);
    EXPECT_EQ(app.delayed.logicalFrames, 0U);
    EXPECT_TRUE(app.frameIds.empty());
    EXPECT_FALSE(std::filesystem::exists(files.output));
    EXPECT_FALSE(std::filesystem::exists(files.root / "capture.png"));
}

TEST(FrameCompletionCapture, FatalResizeErrorStaysLatchedAcrossQueuedRebuilds) {
    CaptureFiles files;
    VulkanFaultScope faults;
    faults.failComputeOnce = true;
    CompletionApp app;
    app.resizeCreationFault = &faults;
    auto cfg = files.config();
    cfg.outputFile.clear(); // interactive event path, not the offscreen loop
    EXPECT_NE(app.run(std::move(cfg)), 0);
    EXPECT_TRUE(faults.computeInjected);
    EXPECT_EQ(app.completedSecondResize, 0U) << "The fatal first rebuild must prevent the queued retry";
}
