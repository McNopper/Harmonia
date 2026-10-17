// H-05 seam-guard rejection tests (pure CPU, no Vulkan device):
//   - SceneExrContract       — the scene-referred capture must be written as
//                              OpenEXR (chromaticities-tagged, float precision).
//   - OutputColorSpaceMapping — every Vulkan surface color space presented must
//                              have a ToneMapper encoding; unmapped ones are
//                              refused, never silently presented as sRGB.
#include <gtest/gtest.h>
#include <optional>
#include <string>

#include "harmonia/presentation/ImageCapture.hpp"
#include "harmonia/presentation/Swapchain.hpp"

// ── Scene-referred EXR container contract (ImageCapture::saveExr guard) ──────

TEST(SceneExrContract, AcceptsTheOpenExrContainer) {
    const auto violation = harmonia::presentation::ImageCapture::sceneExrContainerViolation("capture.exr");
    EXPECT_FALSE(violation.has_value());
}

TEST(SceneExrContract, RejectsNonExrContainers) {
    // PNG: 8-bit display-oriented container — the tonemapped PNG path handles it.
    const auto png = harmonia::presentation::ImageCapture::sceneExrContainerViolation("capture.png");
    ASSERT_TRUE(png.has_value());
    EXPECT_NE(png->find("OpenEXR"), std::string::npos);

    // Radiance .hdr: float-capable but drops the chromaticities metadata.
    const auto hdr = harmonia::presentation::ImageCapture::sceneExrContainerViolation("capture.hdr");
    ASSERT_TRUE(hdr.has_value());
    EXPECT_NE(hdr->find("OpenEXR"), std::string::npos);

    // No resolvable writer at all.
    const auto unknown =
        harmonia::presentation::ImageCapture::sceneExrContainerViolation("capture.definitely_not_a_format");
    ASSERT_TRUE(unknown.has_value());
    EXPECT_NE(unknown->find("OpenEXR"), std::string::npos);
}

// ── Swapchain surface color space → OutputColorSpace mapping ─────────────────

TEST(OutputColorSpaceMapping, MapsEverySupportedSurfaceColorSpace) {
    using harmonia::presentation::mapOutputColorSpace;
    using harmonia::utils::OutputColorSpace;

    const auto cs = mapOutputColorSpace(VK_COLOR_SPACE_HDR10_ST2084_EXT);
    ASSERT_TRUE(cs.has_value());
    EXPECT_EQ(*cs, OutputColorSpace::eHDR10);

    const auto hlg = mapOutputColorSpace(VK_COLOR_SPACE_HDR10_HLG_EXT);
    ASSERT_TRUE(hlg.has_value());
    EXPECT_EQ(*hlg, OutputColorSpace::eHLG);

    const auto scrgb = mapOutputColorSpace(VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT);
    ASSERT_TRUE(scrgb.has_value());
    EXPECT_EQ(*scrgb, OutputColorSpace::eScRGB);

    const auto p3 = mapOutputColorSpace(VK_COLOR_SPACE_DISPLAY_P3_NONLINEAR_EXT);
    ASSERT_TRUE(p3.has_value());
    EXPECT_EQ(*p3, OutputColorSpace::eDisplayP3);

    const auto bt709Linear = mapOutputColorSpace(VK_COLOR_SPACE_BT709_LINEAR_EXT);
    ASSERT_TRUE(bt709Linear.has_value());
    EXPECT_EQ(*bt709Linear, OutputColorSpace::eSDRLinear);

    const auto sdr = mapOutputColorSpace(VK_COLOR_SPACE_SRGB_NONLINEAR_KHR);
    ASSERT_TRUE(sdr.has_value());
    EXPECT_EQ(*sdr, OutputColorSpace::eSDR);
}

// H-05 rejection test: a surface color space without a ToneMapper encoding
// (e.g. Display-P3 LINEAR, reachable via Swapchain::create's formats.front()
// fallback) must be refused, not silently presented through the sRGB path.
TEST(OutputColorSpaceMapping, RejectsUnmappedSurfaceColorSpaces) {
    const auto mapped = harmonia::presentation::mapOutputColorSpace(VK_COLOR_SPACE_DISPLAY_P3_LINEAR_EXT);
    EXPECT_FALSE(mapped.has_value());
}
