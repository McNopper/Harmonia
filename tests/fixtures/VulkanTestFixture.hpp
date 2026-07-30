#ifndef TESTS_FIXTURES_VULKANTESTFIXTURE_HPP
#define TESTS_FIXTURES_VULKANTESTFIXTURE_HPP

// Shared Vulkan test infrastructure for component and module tests.
//
// Usage:
//   1. In main.cpp: call setupVulkanTestContext() before RUN_ALL_TESTS(),
//      and teardownVulkanTestContext() after.
//   2. Derive your test fixture from VulkanFixture (basic Vulkan) or
//      RtFixture (also checks RT extensions are present).
//
// One harmonia::Context is created per test binary; tests share it for speed.
// TearDown() calls vkDeviceWaitIdle so each test starts with a quiescent GPU.

#include <volk/volk.h>

#include <gtest/gtest.h>
#include <memory>

#include "harmonia/core/CommandPool.hpp"
#include "harmonia/vulkan_init/Context.hpp"
#include "harmonia/vulkan_init/PhysicalDevice.hpp"

struct VulkanTestContext {
    SDL_Window* window{};
    std::unique_ptr<harmonia::Context> context;
    std::unique_ptr<harmonia::CommandPool> commandPool;

    [[nodiscard]] bool isValid() const noexcept { return context && commandPool; }
    [[nodiscard]] const harmonia::DeviceContext& deviceCtx() const noexcept { return context->deviceContext(); }
    [[nodiscard]] const harmonia::PhysicalDeviceInfo& physInfo() const noexcept { return context->physicalDeviceInfo(); }
};

// Set in main() before RUN_ALL_TESTS(); nullptr means Vulkan unavailable.
inline VulkanTestContext* g_vulkanTestCtx = nullptr;

// Base fixture: requires a valid Vulkan context (which in this project always
// includes RT because harmonia::Context::create() selects only RT-capable devices).
class VulkanFixture : public ::testing::Test {
  protected:
    void SetUp() override {
        if (g_vulkanTestCtx == nullptr || !g_vulkanTestCtx->isValid()) {
            GTEST_SKIP() << "Vulkan context unavailable";
        }
    }

    void TearDown() override {
        if (g_vulkanTestCtx != nullptr && g_vulkanTestCtx->isValid()) {
            vkDeviceWaitIdle(g_vulkanTestCtx->deviceCtx().device);
        }
    }

    [[nodiscard]] const harmonia::DeviceContext& deviceCtx() const noexcept { return g_vulkanTestCtx->deviceCtx(); }
    [[nodiscard]] harmonia::CommandPool& commandPool() const noexcept { return *g_vulkanTestCtx->commandPool; }
};

// Extended fixture: also skips if RT extension functions are not loaded.
// Use this for tests that exercise harmonia::AccelerationStructure / SBT / PathTracer.
class RtFixture : public VulkanFixture {
  protected:
    void SetUp() override {
        VulkanFixture::SetUp();
        if (vkCmdTraceRaysKHR == nullptr) {
            GTEST_SKIP() << "Ray tracing not available on this device";
        }
    }

    [[nodiscard]] const harmonia::PhysicalDeviceInfo& physInfo() const noexcept { return g_vulkanTestCtx->physInfo(); }
};
#endif // TESTS_FIXTURES_VULKANTESTFIXTURE_HPP
