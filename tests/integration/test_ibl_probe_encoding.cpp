// H-05 seam-guard rejection tests for the IBL input encoding contract.
//
// The environment panorama must be scene-referred LINEAR OpenEXR. These tests
// exercise the guard in IblProbe::readEXR — which fires BEFORE any GPU work —
// with a default-constructed (invalid) DeviceContext: rejection cases return
// VK_ERROR_FORMAT_NOT_SUPPORTED without ever touching Vulkan, so they run on
// any machine. (Full load + upload of a valid panorama needs a real device and
// lives in the GPU-backed suites.)
//
// Fixtures are written with OpenImageIO (linked transitively via
// harmonia::passes): a PNG container, an sRGB-tagged EXR and a plain linear EXR.
#include <OpenImageIO/imageio.h>
#include <array>
#include <cstdint>
#include <filesystem>
#include <gtest/gtest.h>
#include <string>

#include "harmonia/DeviceContext.hpp"
#include "harmonia/core/CommandPool.hpp"
#include "harmonia/scene/IblProbe.hpp"

namespace {

/// Write a tiny 2x2 image via OpenImageIO so the guard under test actually
/// opens a well-formed file (OIIO resolves the reader from the extension).
/// @p dataType must be supported by the target container (PNG = UINT8,
/// EXR = FLOAT).
[[nodiscard]] bool
writeTestImage(const std::filesystem::path& path, const char* oiioColorSpace, OIIO::TypeDesc dataType) {
    auto out = OIIO::ImageOutput::create(path.string());
    if (!out) {
        return false;
    }
    OIIO::ImageSpec spec(2, 2, 3, dataType);
    if (oiioColorSpace != nullptr && oiioColorSpace[0] != '\0') {
        spec.attribute("oiio:ColorSpace", oiioColorSpace);
    }
    if (!out->open(path.string(), spec)) {
        return false;
    }
    // Pixel storage must match the declared write type (no conversion here);
    // all-zero pixels are a valid linear black panorama.
    const std::array<std::uint8_t, 12> pixels8{};
    const std::array<float, 12> pixelsF{};
    const bool isUint8 = (dataType == OIIO::TypeDesc::UINT8);
    if (!out->write_image(
            dataType, isUint8 ? static_cast<const void*>(pixels8.data()) : static_cast<const void*>(pixelsF.data()))) {
        return false;
    }
    out->close();
    return true;
}

[[nodiscard]] std::filesystem::path fixtureDir() {
    return std::filesystem::temp_directory_path() / "harmonia_ibl_encoding_guard";
}

} // namespace

TEST(IblProbeEncoding, DisplayReferredContainerIsRejected) {
    std::filesystem::create_directories(fixtureDir());
    const std::filesystem::path path = fixtureDir() / "sky.png";
    ASSERT_TRUE(writeTestImage(path, nullptr, OIIO::TypeDesc::UINT8));

    const harmonia::DeviceContext dummyCtx{};
    const harmonia::core::CommandPool dummyPool{};
    const auto probe = harmonia::scene::IblProbe::loadFromEXR(dummyCtx, dummyPool, path);

    ASSERT_FALSE(probe.has_value());
    EXPECT_EQ(probe.error(), VK_ERROR_FORMAT_NOT_SUPPORTED);

    std::filesystem::remove_all(fixtureDir());
}

TEST(IblProbeEncoding, NonLinearTaggedExrIsRejected) {
    std::filesystem::create_directories(fixtureDir());
    const std::filesystem::path path = fixtureDir() / "sky_srgb.exr";
    ASSERT_TRUE(writeTestImage(path, "sRGB", OIIO::TypeDesc::FLOAT));

    const harmonia::DeviceContext dummyCtx{};
    const harmonia::core::CommandPool dummyPool{};
    const auto probe = harmonia::scene::IblProbe::loadFromEXR(dummyCtx, dummyPool, path);

    ASSERT_FALSE(probe.has_value());
    EXPECT_EQ(probe.error(), VK_ERROR_FORMAT_NOT_SUPPORTED);

    std::filesystem::remove_all(fixtureDir());
}

// Positive control: a plain linear EXR (EXR defaults to linear when
// oiio:ColorSpace is absent) must pass the encoding guard. With the dummy
// device the load then fails at the GPU upload — the assertion pins that the
// failure is NOT the encoding rejection.
TEST(IblProbeEncoding, LinearExrPassesTheEncodingGuard) {
    std::filesystem::create_directories(fixtureDir());
    const std::filesystem::path path = fixtureDir() / "sky_linear.exr";
    ASSERT_TRUE(writeTestImage(path, nullptr, OIIO::TypeDesc::FLOAT));

    const harmonia::DeviceContext dummyCtx{};
    const harmonia::core::CommandPool dummyPool{};
    const auto probe = harmonia::scene::IblProbe::loadFromEXR(dummyCtx, dummyPool, path);

    ASSERT_FALSE(probe.has_value()); // upload fails on the dummy device — expected
    EXPECT_NE(probe.error(), VK_ERROR_FORMAT_NOT_SUPPORTED);

    std::filesystem::remove_all(fixtureDir());
}
