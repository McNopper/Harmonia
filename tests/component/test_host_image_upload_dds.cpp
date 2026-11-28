// Component tests: the precomputed mip chain of harmonia::core::uploadHostImage and the
// block-compressed DDS path of scene::Texture::loadFromFile.
//
// What is proven, and by which oracle:
//   - the BC7 upload decodes on the GPU to the uncompressed reference within the stated BC7
//     error (reference: the source image through the unchanged RGBA8 path, Texture::create);
//   - every level of the chain is readable and holds the right blocks: the GPU decode of
//     each level matches DirectXTex's own CPU decode of the same file within 1 LSB
//     (an independent decoder, so a wrong level offset or block extent is caught exactly);
//   - the hardware sRGB decode is applied only when the scene asks for it;
//   - rejections: a level range that is a block short, a wrong level extent, too many levels,
//     an unknown format, and the colour-space rules a compressed texture cannot represent;
//   - the single-level RGBA8 path is unchanged.
//
// The BC7 fixtures are made with DirectXTex texconv (tests/assets/dds/README.md).

#include <volk/volk.h>

#include <OpenImageIO/imageio.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <slang-math/slang-math.hpp>
#include <span>
#include <string>
#include <vector>

#include "fixtures/VulkanTestFixture.hpp"
#include "harmonia/core/Buffer.hpp"
#include "harmonia/core/DdsImage.hpp"
#include "harmonia/core/DescriptorBufferWriter.hpp"
#include "harmonia/core/HostImageUpload.hpp"
#include "harmonia/core/Sampler.hpp"
#include "harmonia/core/ShaderModule.hpp"
#include "harmonia/core/VulkanHandle.hpp"
#include "harmonia/scene/Texture.hpp"
#include "harmonia/utils/ColorSpace.hpp"

namespace {

using harmonia::core::Buffer;
using harmonia::core::DdsImage;
using harmonia::core::HostImageMipLevel;
using harmonia::scene::Texture;
using harmonia::scene::TextureColorSpace;
using harmonia::utils::ColorSpace::WorkingColorSpace;

constexpr std::uint32_t kSize = 16U;
constexpr std::uint32_t kLevels = 5U;

// Stated BC7 error of the fixture at level 0 against its uncompressed source, measured
// with DirectXTex 2026.5.8.1 (CPU BC7 codec): maximum 76/255 per channel, mean 4.83/255.
// The fixture is hard content on purpose (a wrapping gradient, saturated patches, a
// checker), so the bound is loose; the per-level oracle below is the tight one.
constexpr float kBc7MaxError = 80.0F / 255.0F;
constexpr float kBc7MeanError = 6.0F / 255.0F;
// Two decoders of the same blocks agree to rounding: 1 LSB, plus slack for float noise.
constexpr float kDecoderError = 1.5F / 255.0F;
// The hardware sRGB decode against Harmonia's own decode of the 8-bit reference.
constexpr float kSrgbError = 2.0F / 255.0F;

std::filesystem::path fixture(const char* file) {
    return std::filesystem::path(HARMONIA_TEST_ASSETS_DIR) / "dds" / file;
}

std::vector<std::byte> readFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    EXPECT_TRUE(file.is_open()) << path.string();
    const auto end = file.tellg();
    std::vector<std::byte> bytes(static_cast<std::size_t>(end));
    file.seekg(0, std::ios::beg);
    if (!bytes.empty()) {
        file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        EXPECT_TRUE(file.good()) << path.string();
    }
    return bytes;
}

std::uint32_t read32(std::span<const std::byte> bytes, std::size_t offset) {
    std::uint32_t value = 0U;
    for (std::size_t index = 0U; index < 4U; ++index) {
        value |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + index])) << (8U * index);
    }
    return value;
}

struct Rgba {
    std::uint8_t r{};
    std::uint8_t g{};
    std::uint8_t b{};
    std::uint8_t a{};
};

/// The uncompressed source image, read exactly as the PNG/TIFF path reads it (OIIO, 3
/// channels into an opaque RGBA buffer).
std::vector<Rgba> readSource(const std::filesystem::path& path) {
    auto input = OIIO::ImageInput::open(path.string());
    EXPECT_TRUE(input != nullptr) << OIIO::geterror();
    if (input == nullptr) {
        return {};
    }
    const OIIO::ImageSpec& spec = input->spec();
    EXPECT_EQ(spec.width, static_cast<int>(kSize));
    EXPECT_EQ(spec.height, static_cast<int>(kSize));
    std::vector<Rgba> pixels(static_cast<std::size_t>(spec.width) * static_cast<std::size_t>(spec.height),
                             Rgba{0xFF, 0xFF, 0xFF, 0xFF});
    const bool read = input->read_image(0,
                                        0,
                                        0,
                                        std::min(spec.nchannels, 3),
                                        OIIO::TypeDesc::UINT8,
                                        pixels.data(),
                                        static_cast<OIIO::stride_t>(sizeof(Rgba)));
    EXPECT_TRUE(read) << input->geterror();
    return pixels;
}

/// One mip level of DirectXTex's decode of the BC7 fixture: an uncompressed
/// R8G8B8A8_UNORM DDS with the whole chain, read with a reader written here from the
/// format definition (independent of harmonia::core::DdsImage).
struct ReferenceLevel {
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<Rgba> pixels;
};

std::vector<ReferenceLevel> readReferenceChain(const std::filesystem::path& path) {
    const std::vector<std::byte> bytes = readFile(path);
    const std::span<const std::byte> file{bytes};
    constexpr std::uint32_t kMagic = 0x20534444U;
    constexpr std::uint32_t kDx10 = 0x30315844U;
    constexpr std::uint32_t kR8G8B8A8Unorm = 28U;
    EXPECT_EQ(file.size(), 1512U);
    if (file.size() < 148U) {
        return {};
    }
    EXPECT_EQ(read32(file, 0U), kMagic);
    EXPECT_EQ(read32(file, 84U), kDx10);
    EXPECT_EQ(read32(file, 128U), kR8G8B8A8Unorm);
    const std::uint32_t height = read32(file, 12U);
    const std::uint32_t width = read32(file, 16U);
    const std::uint32_t mips = read32(file, 28U);
    EXPECT_EQ(width, kSize);
    EXPECT_EQ(height, kSize);
    EXPECT_EQ(mips, kLevels);
    if (width != kSize || height != kSize || mips == 0U || mips > 32U) {
        return {};
    }
    std::vector<ReferenceLevel> levels;
    std::size_t offset = 148U;
    std::uint32_t levelWidth = width;
    std::uint32_t levelHeight = height;
    for (std::uint32_t level = 0U; level < mips; ++level) {
        ReferenceLevel reference;
        reference.width = levelWidth;
        reference.height = levelHeight;
        if (offset + static_cast<std::size_t>(levelWidth) * levelHeight * 4U > file.size()) {
            ADD_FAILURE() << "reference level " << level << " runs past the end of the file";
            return {};
        }
        for (std::uint32_t index = 0U; index < levelWidth * levelHeight; ++index) {
            const std::size_t at = offset + static_cast<std::size_t>(index) * 4U;
            reference.pixels.push_back(Rgba{std::to_integer<std::uint8_t>(file[at]),
                                            std::to_integer<std::uint8_t>(file[at + 1U]),
                                            std::to_integer<std::uint8_t>(file[at + 2U]),
                                            std::to_integer<std::uint8_t>(file[at + 3U])});
        }
        levels.push_back(std::move(reference));
        offset += static_cast<std::size_t>(levelWidth) * levelHeight * 4U;
        levelWidth = std::max(1U, levelWidth >> 1U);
        levelHeight = std::max(1U, levelHeight >> 1U);
    }
    EXPECT_EQ(offset, file.size());
    return levels;
}

/// The push constants of texture_level_readback_test.slang.
struct ReadbackPush {
    std::uint32_t level{};
    std::uint32_t width{};
    std::uint32_t height{};
};

/// A compute pipeline that copies one mip level of a 2D image into a float4 buffer.
class LevelReader {
  public:
    [[nodiscard]] static std::expected<LevelReader, std::string> create(const harmonia::DeviceContext& ctx) {
        const std::array<VkDescriptorSetLayoutBinding, 2> bindings{{
            {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        }};
        const VkDescriptorSetLayoutCreateInfo setLayoutInfo{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .pNext = nullptr,
            .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT,
            .bindingCount = static_cast<std::uint32_t>(bindings.size()),
            .pBindings = bindings.data(),
        };
        VkDescriptorSetLayout setLayoutRaw{};
        if (vkCreateDescriptorSetLayout(ctx.device, &setLayoutInfo, nullptr, &setLayoutRaw) != VK_SUCCESS) {
            return std::unexpected("set layout");
        }
        LevelReader reader;
        reader.setLayout = harmonia::core::UniqueDescriptorSetLayout{ctx.device, setLayoutRaw};

        const VkPushConstantRange pushRange{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ReadbackPush)};
        const VkPipelineLayoutCreateInfo layoutInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .setLayoutCount = 1,
            .pSetLayouts = &setLayoutRaw,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &pushRange,
        };
        VkPipelineLayout layoutRaw{};
        if (vkCreatePipelineLayout(ctx.device, &layoutInfo, nullptr, &layoutRaw) != VK_SUCCESS) {
            return std::unexpected("pipeline layout");
        }
        reader.layout = harmonia::core::UniquePipelineLayout{ctx.device, layoutRaw};

        auto module = harmonia::core::createShaderModule(
            ctx.device, std::filesystem::path(HARMONIA_TEST_SHADER_DIR) / "texture_level_readback_test.spv");
        if (!module) {
            return std::unexpected("shader module: texture_level_readback_test.spv");
        }
        const VkComputePipelineCreateInfo pipelineInfo{
            .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .pNext = nullptr,
            .flags = VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT,
            .stage =
                VkPipelineShaderStageCreateInfo{
                    .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                    .pNext = nullptr,
                    .flags = 0,
                    .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                    .module = *module,
                    .pName = "main",
                    .pSpecializationInfo = nullptr,
                },
            .layout = layoutRaw,
            .basePipelineHandle = VK_NULL_HANDLE,
            .basePipelineIndex = 0,
        };
        VkPipeline pipelineRaw{};
        const VkResult pipelineResult =
            vkCreateComputePipelines(ctx.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipelineRaw);
        vkDestroyShaderModule(ctx.device, *module, nullptr);
        if (pipelineResult != VK_SUCCESS) {
            return std::unexpected("pipeline");
        }
        reader.pipeline = harmonia::core::UniquePipeline{ctx.device, pipelineRaw};

        if (!reader.writer.init(ctx,
                                setLayoutRaw,
                                static_cast<std::uint32_t>(bindings.size()),
                                "test.textureLevelReadback.descriptors")) {
            return std::unexpected("descriptor buffer");
        }
        VkSamplerCreateInfo samplerInfo =
            harmonia::core::makeSamplerCreateInfo({.magFilter = VK_FILTER_NEAREST,
                                                   .minFilter = VK_FILTER_NEAREST,
                                                   .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST});
        VkSampler sampler = VK_NULL_HANDLE;
        if (vkCreateSampler(ctx.device, &samplerInfo, nullptr, &sampler) != VK_SUCCESS) {
            return std::unexpected("sampler");
        }
        reader.sampler = harmonia::core::UniqueSampler{ctx.device, sampler};
        return reader;
    }

    /// The level @p level of @p view, row-major from the top left. Writes the descriptors
    /// of this read into the writer, hence not const.
    [[nodiscard]] std::vector<sm::float4> read(const harmonia::DeviceContext& ctx,
                                               harmonia::core::CommandPool& pool,
                                               VkImageView view,
                                               std::uint32_t level,
                                               std::uint32_t width,
                                               std::uint32_t height) {
        const std::size_t count = static_cast<std::size_t>(width) * height;
        auto output = Buffer::create(ctx,
                                     static_cast<VkDeviceSize>(count) * sizeof(sm::float4),
                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                     VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                     "test.textureLevelReadback.output");
        EXPECT_TRUE(output.has_value()) << "output buffer: VkResult " << static_cast<int>(output.error());
        if (!output.has_value()) {
            return {};
        }
        writer.writeCombinedImageSampler(ctx, 0U, 0U, sampler.get(), view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        writer.writeStorageBuffer(ctx, 1, output->deviceAddress(), output->size());

        auto cmd = pool.beginOneShot();
        EXPECT_TRUE(cmd.has_value());
        if (!cmd.has_value()) {
            return {};
        }
        vkCmdBindPipeline(*cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.get());
        writer.bind(*cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout.get());
        const ReadbackPush push{level, width, height};
        vkCmdPushConstants(*cmd, layout.get(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(*cmd, (width + 7U) / 8U, (height + 7U) / 8U, 1U);
        const VkMemoryBarrier2 toHost{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
            .pNext = nullptr,
            .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
            .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT,
        };
        const VkDependencyInfo dependency{
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .pNext = nullptr,
            .dependencyFlags = 0,
            .memoryBarrierCount = 1,
            .pMemoryBarriers = &toHost,
            .bufferMemoryBarrierCount = 0,
            .pBufferMemoryBarriers = nullptr,
            .imageMemoryBarrierCount = 0,
            .pImageMemoryBarriers = nullptr,
        };
        vkCmdPipelineBarrier2(*cmd, &dependency);
        EXPECT_EQ(pool.endOneShot(*cmd), VK_SUCCESS);

        std::vector<sm::float4> texels(count);
        EXPECT_EQ(output->readData(texels.data(), output->size()), VK_SUCCESS);
        return texels;
    }

    harmonia::core::UniqueDescriptorSetLayout setLayout;
    harmonia::core::UniquePipelineLayout layout;
    harmonia::core::UniquePipeline pipeline;
    harmonia::core::UniqueSampler sampler;
    harmonia::core::DescriptorBufferWriter writer;
};

sm::float4 toFloat(const Rgba& pixel) {
    return sm::float4{static_cast<float>(pixel.r) / 255.0F,
                      static_cast<float>(pixel.g) / 255.0F,
                      static_cast<float>(pixel.b) / 255.0F,
                      static_cast<float>(pixel.a) / 255.0F};
}

struct Error {
    float max{};  ///< largest absolute channel difference
    float mean{}; ///< mean absolute channel difference
};

/// Channel @p channel (0..3) of a float4 by name, whatever the math library offers.
float component(const sm::float4& value, int channel) {
    switch (channel) {
    case 0:
        return value.x;
    case 1:
        return value.y;
    case 2:
        return value.z;
    default:
        return value.w;
    }
}

/// Difference of the first @p channels of two RGBA images.
Error compare(const std::vector<sm::float4>& actual, const std::vector<Rgba>& expected, int channels) {
    EXPECT_EQ(actual.size(), expected.size());
    const std::size_t count = std::min(actual.size(), expected.size());
    Error error;
    for (std::size_t index = 0U; index < count; ++index) {
        const sm::float4 reference = toFloat(expected[index]);
        for (int channel = 0; channel < channels; ++channel) {
            const float difference = std::abs(component(actual[index], channel) - component(reference, channel));
            error.max = std::max(error.max, difference);
            error.mean += difference;
        }
    }
    error.mean /= static_cast<float>(count) * static_cast<float>(channels);
    return error;
}

std::vector<HostImageMipLevel> mipLevelsOf(const DdsImage& image) {
    std::vector<HostImageMipLevel> levels;
    for (const DdsImage::Level& level : image.levels()) {
        const std::span<const std::byte> bytes = image.levelBytes(level);
        levels.push_back(HostImageMipLevel{
            .data = bytes.data(),
            .byteLength = bytes.size(),
            .width = level.width,
            .height = level.height,
        });
    }
    return levels;
}

harmonia::core::SamplerSpec linearSampler() {
    harmonia::core::SamplerSpec sampler{};
    sampler.anisotropyEnable = VK_TRUE;
    return sampler; // linear, linear mip, CLAMP_TO_EDGE: the defaults
}

/// The stages the upload must make visible, as the scene passes them.
constexpr VkPipelineStageFlags2 kStages =
    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;

class HostImageUploadDds : public VulkanFixture {
  protected:
    std::expected<LevelReader, std::string> reader;

    void SetUp() override {
        VulkanFixture::SetUp();
        if (g_vulkanTestCtx == nullptr || !g_vulkanTestCtx->isValid()) {
            return;
        }
        reader = LevelReader::create(deviceCtx());
        ASSERT_TRUE(reader.has_value()) << reader.error();
    }

    std::vector<sm::float4> readLevel(const Texture& texture, std::uint32_t level) {
        const std::uint32_t width = std::max(1U, texture.width() >> level);
        const std::uint32_t height = std::max(1U, texture.height() >> level);
        return reader->read(deviceCtx(), commandPool(), texture.image().view(), level, width, height);
    }
};

std::filesystem::path scratchFile(const char* name) {
    const std::filesystem::path directory = std::filesystem::temp_directory_path() / "harmonia_dds_upload_tests";
    std::filesystem::create_directories(directory);
    return directory / name;
}

void writeFile(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(file.is_open()) << path.string();
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    ASSERT_TRUE(file.good()) << path.string();
}

void writeGray16Tiff(const std::filesystem::path& path, std::span<const std::uint16_t> pixels) {
    constexpr int kWidth = 4;
    constexpr int kHeight = 4;
    ASSERT_EQ(pixels.size(), static_cast<std::size_t>(kWidth * kHeight));
    auto output = OIIO::ImageOutput::create(path.string());
    ASSERT_TRUE(output != nullptr) << OIIO::geterror();
    const OIIO::ImageSpec spec(kWidth, kHeight, 1, OIIO::TypeDesc::UINT16);
    ASSERT_TRUE(output->open(path.string(), spec)) << output->geterror();
    ASSERT_TRUE(output->write_image(OIIO::TypeDesc::UINT16, pixels.data())) << output->geterror();
    ASSERT_TRUE(output->close()) << output->geterror();
}

} // namespace

// ── The compressed upload ──────────────────────────────────────────────────────

// A BC7 DDS goes up as BC7 with its precomputed chain and decodes on the GPU to the
// uncompressed reference within the stated BC7 error.
TEST_F(HostImageUploadDds, Bc7UploadStaysCompressedAndMatchesTheUncompressedReference) {
    auto compressed =
        Texture::loadFromFile(deviceCtx(), commandPool(), fixture("bc7_checker16.dds"), TextureColorSpace::Data);
    ASSERT_TRUE(compressed.has_value()) << "VkResult " << static_cast<int>(compressed.error());
    // Not decoded, not re-uploaded: the image has the format of the file's blocks.
    EXPECT_EQ(compressed->image().format(), VK_FORMAT_BC7_UNORM_BLOCK);
    EXPECT_NE(compressed->image().view(), VK_NULL_HANDLE);
    EXPECT_EQ(compressed->width(), kSize);
    EXPECT_EQ(compressed->height(), kSize);
    EXPECT_EQ(compressed->mipLevels(), kLevels);

    const std::vector<Rgba> source = readSource(fixture("bc7_checker16.tga"));
    ASSERT_EQ(source.size(), kSize * kSize);
    const auto referenceBytes = std::as_bytes(std::span<const Rgba>{source});
    auto reference = Texture::create(deviceCtx(), commandPool(), referenceBytes, kSize, kSize, "test.dds.reference");
    ASSERT_TRUE(reference.has_value()) << "VkResult " << static_cast<int>(reference.error());
    EXPECT_EQ(reference->image().format(), VK_FORMAT_R8G8B8A8_UNORM);

    const std::vector<sm::float4> referenceLevel0 = readLevel(*reference, 0U);
    const std::vector<sm::float4> compressedLevel0 = readLevel(*compressed, 0U);
    // Positive control of the oracle: the RGBA8 path returns the source exactly.
    const Error exact = compare(referenceLevel0, source, 4);
    EXPECT_EQ(exact.max, 0.0F);

    const Error bc7 = compare(compressedLevel0, source, 4);
    EXPECT_LE(bc7.max, kBc7MaxError) << "BC7 error above the stated bound";
    EXPECT_LE(bc7.mean, kBc7MeanError) << "BC7 mean error above the stated bound";
}

// All five levels are readable and hold the blocks of their own level: the GPU decode of
// each level matches DirectXTex's CPU decode of the same file. A level copied from the
// wrong offset, with the wrong block extent, or left undefined cannot pass this.
TEST_F(HostImageUploadDds, EveryLevelHoldsItsOwnBlocks) {
    auto compressed =
        Texture::loadFromFile(deviceCtx(), commandPool(), fixture("bc7_checker16.dds"), TextureColorSpace::Data);
    ASSERT_TRUE(compressed.has_value()) << "VkResult " << static_cast<int>(compressed.error());
    ASSERT_EQ(compressed->mipLevels(), kLevels);

    const std::vector<ReferenceLevel> chain = readReferenceChain(fixture("bc7_checker16_ref.dds"));
    ASSERT_EQ(chain.size(), kLevels);
    for (std::uint32_t level = 0U; level < kLevels; ++level) {
        SCOPED_TRACE(::testing::Message() << "level " << level);
        const std::vector<sm::float4> texels = readLevel(*compressed, level);
        ASSERT_EQ(texels.size(), chain[level].pixels.size());
        const Error error = compare(texels, chain[level].pixels, 4);
        EXPECT_LE(error.max, kDecoderError) << "GPU decode differs from the DirectXTex decode";
    }
}

// The *_SRGB variant makes the hardware decode sRGB, and that is the decode the CPU path
// would have done. Nothing else is converted.
TEST_F(HostImageUploadDds, HardwareSrgbDecodeIsAppliedWhenTheSceneAsksForIt) {
    auto texture = Texture::loadFromFile(deviceCtx(),
                                         commandPool(),
                                         fixture("bc7_checker16_srgb.dds"),
                                         TextureColorSpace::SrgbRec709Scene,
                                         WorkingColorSpace::LinRec709);
    ASSERT_TRUE(texture.has_value()) << "VkResult " << static_cast<int>(texture.error());
    EXPECT_EQ(texture->image().format(), VK_FORMAT_BC7_SRGB_BLOCK);

    const std::vector<ReferenceLevel> chain = readReferenceChain(fixture("bc7_checker16_ref.dds"));
    ASSERT_EQ(chain.size(), kLevels);
    std::vector<Rgba> decoded;
    decoded.reserve(chain[0].pixels.size());
    for (const Rgba& pixel : chain[0].pixels) {
        const sm::float3 linear =
            harmonia::utils::ColorSpace::srgbToLinearRec709(sm::float3{static_cast<float>(pixel.r) / 255.0F,
                                                                       static_cast<float>(pixel.g) / 255.0F,
                                                                       static_cast<float>(pixel.b) / 255.0F});
        const auto toByte = [](float value) {
            return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F));
        };
        decoded.push_back(Rgba{toByte(linear.r), toByte(linear.g), toByte(linear.b), pixel.a});
    }
    const Error error = compare(readLevel(*texture, 0U), decoded, 4);
    EXPECT_LE(error.max, kSrgbError);
}

// The same file read as data stays what it is: no decode, the linear variant.
TEST_F(HostImageUploadDds, ALinearDeclarationKeepsTheBlocksLinear) {
    auto texture = Texture::loadFromFile(deviceCtx(),
                                         commandPool(),
                                         fixture("bc7_checker16.dds"),
                                         TextureColorSpace::LinRec2020Scene,
                                         WorkingColorSpace::LinRec2020);
    ASSERT_TRUE(texture.has_value()) << "VkResult " << static_cast<int>(texture.error());
    EXPECT_EQ(texture->image().format(), VK_FORMAT_BC7_UNORM_BLOCK);
    const std::vector<ReferenceLevel> chain = readReferenceChain(fixture("bc7_checker16_ref.dds"));
    ASSERT_EQ(chain.size(), kLevels);
    const Error error = compare(readLevel(*texture, 0U), chain[0].pixels, 4);
    EXPECT_LE(error.max, kDecoderError);
}

// ── High-precision non-DDS sources ───────────────────────────────────────────────

TEST_F(HostImageUploadDds, Unconverted16BitDataKeepsEverySourceCodeValue) {
    const std::array<std::uint16_t, 16> source{
        1, 2, 255, 256, 257, 511, 512, 4097, 16384, 32768, 32769, 49152, 65533, 65534, 65535, 128};
    const std::filesystem::path path = scratchFile("precision_data16.tif");
    writeGray16Tiff(path, source);
    auto texture = Texture::loadFromFile(deviceCtx(), commandPool(), path, TextureColorSpace::Data);
    ASSERT_TRUE(texture.has_value()) << "VkResult " << static_cast<int>(texture.error());
    EXPECT_EQ(texture->image().format(), VK_FORMAT_R16G16B16A16_UNORM);
    EXPECT_EQ(texture->width(), 4U);
    EXPECT_EQ(texture->height(), 4U);
    const std::vector<sm::float4> actual = readLevel(*texture, 0U);
    ASSERT_EQ(actual.size(), source.size());
    for (std::size_t i = 0; i < source.size(); ++i) {
        SCOPED_TRACE(::testing::Message() << "texel " << i << ", source " << source[i]);
        EXPECT_NEAR(actual[i].x, static_cast<float>(source[i]) / 65535.0F, 1.0e-7F);
        EXPECT_FLOAT_EQ(actual[i].w, 1.0F);
    }
    // The first two values are distinct here; both become zero in the former UINT8 path.
    EXPECT_NE(actual[0].x, actual[1].x);
    std::filesystem::remove(path);
}

TEST_F(HostImageUploadDds, Converted16BitSrgbUsesLinearHalfWithoutAn8BitIntermediate) {
    const std::array<std::uint16_t, 16> source{
        1, 2, 255, 256, 257, 511, 512, 4097, 16384, 32768, 32769, 49152, 65533, 65534, 65535, 128};
    const std::filesystem::path path = scratchFile("precision_srgb16.tif");
    writeGray16Tiff(path, source);
    auto texture = Texture::loadFromFile(
        deviceCtx(), commandPool(), path, TextureColorSpace::SrgbRec709Scene, WorkingColorSpace::LinRec709);
    ASSERT_TRUE(texture.has_value()) << "VkResult " << static_cast<int>(texture.error());
    EXPECT_EQ(texture->image().format(), VK_FORMAT_R16G16B16A16_SFLOAT);
    const std::vector<sm::float4> actual = readLevel(*texture, 0U);
    ASSERT_EQ(actual.size(), source.size());
    for (std::size_t i = 0; i < source.size(); ++i) {
        SCOPED_TRACE(::testing::Message() << "texel " << i << ", source " << source[i]);
        const float encoded = static_cast<float>(source[i]) / 65535.0F;
        const float expected = harmonia::utils::ColorSpace::srgbToLinearRec709({encoded, encoded, encoded}).r;
        // Half has a 10-bit fraction: at most half an ULP in [0.5, 1] is 2^-12.
        EXPECT_NEAR(actual[i].x, expected, 2.5e-4F);
    }
    std::filesystem::remove(path);
}

// ── Rejections ─────────────────────────────────────────────────────────────────

// Colour conversions a compressed texture cannot represent are refused, never applied
// afterwards. A mismatch between the file's encoding and the scene's is refused too.
TEST_F(HostImageUploadDds, RefusesConversionsACompressedTextureCannotRepresent) {
    // sRGB Rec.709 data in a Rec.2020 working space needs a primaries conversion.
    const auto primaries = Texture::loadFromFile(deviceCtx(),
                                                 commandPool(),
                                                 fixture("bc7_checker16.dds"),
                                                 TextureColorSpace::SrgbRec709Scene,
                                                 WorkingColorSpace::LinRec2020);
    ASSERT_FALSE(primaries.has_value());
    EXPECT_EQ(primaries.error(), VK_ERROR_FORMAT_NOT_SUPPORTED);

    // Linear Rec.709 data in a Rec.2020 working space, and the other way round.
    const auto to2020 = Texture::loadFromFile(deviceCtx(),
                                              commandPool(),
                                              fixture("bc7_checker16.dds"),
                                              TextureColorSpace::LinRec709Scene,
                                              WorkingColorSpace::LinRec2020);
    ASSERT_FALSE(to2020.has_value());
    EXPECT_EQ(to2020.error(), VK_ERROR_FORMAT_NOT_SUPPORTED);
    const auto to709 = Texture::loadFromFile(deviceCtx(),
                                             commandPool(),
                                             fixture("bc7_checker16.dds"),
                                             TextureColorSpace::LinRec2020Scene,
                                             WorkingColorSpace::LinRec709);
    ASSERT_FALSE(to709.has_value());
    EXPECT_EQ(to709.error(), VK_ERROR_FORMAT_NOT_SUPPORTED);

    // A file stored as sRGB that the scene declares data or linear contradicts itself.
    const auto asData =
        Texture::loadFromFile(deviceCtx(), commandPool(), fixture("bc7_checker16_srgb.dds"), TextureColorSpace::Data);
    ASSERT_FALSE(asData.has_value());
    EXPECT_EQ(asData.error(), VK_ERROR_FORMAT_NOT_SUPPORTED);
    const auto asLinear = Texture::loadFromFile(deviceCtx(),
                                                commandPool(),
                                                fixture("bc7_checker16_srgb.dds"),
                                                TextureColorSpace::LinRec709Scene,
                                                WorkingColorSpace::LinRec709);
    ASSERT_FALSE(asLinear.has_value());
    EXPECT_EQ(asLinear.error(), VK_ERROR_FORMAT_NOT_SUPPORTED);

    // BC5 has no sRGB variant: the decode cannot be done by the hardware.
    const auto bc5 = Texture::loadFromFile(deviceCtx(),
                                           commandPool(),
                                           fixture("bc7_checker16_bc5.dds"),
                                           TextureColorSpace::SrgbRec709Scene,
                                           WorkingColorSpace::LinRec709);
    ASSERT_FALSE(bc5.has_value());
    EXPECT_EQ(bc5.error(), VK_ERROR_FORMAT_NOT_SUPPORTED);
}

// A DDS is never handed to OIIO: a file that is not a DDS fails as a DDS even when OIIO
// could open it, and a truncated one is refused instead of uploading undefined blocks.
TEST_F(HostImageUploadDds, RejectsDamagedDdsFilesInsteadOfDecodingThem) {
    const std::vector<std::byte> source = readFile(fixture("bc7_checker16.tga"));
    ASSERT_FALSE(source.empty());
    const std::filesystem::path disguised = scratchFile("not_a_dds.dds");
    writeFile(disguised, source);
    const auto notDds = Texture::loadFromFile(deviceCtx(), commandPool(), disguised, TextureColorSpace::Data);
    ASSERT_FALSE(notDds.has_value());
    EXPECT_EQ(notDds.error(), VK_ERROR_INITIALIZATION_FAILED);

    const std::vector<std::byte> bc7 = readFile(fixture("bc7_checker16.dds"));
    ASSERT_EQ(bc7.size(), 516U);
    // Negative control for a bad block count: the last level is one block short.
    const std::filesystem::path truncated = scratchFile("truncated.dds");
    writeFile(truncated, std::span<const std::byte>{bc7}.first(bc7.size() - 16U));
    const auto shortFile = Texture::loadFromFile(deviceCtx(), commandPool(), truncated, TextureColorSpace::Data);
    ASSERT_FALSE(shortFile.has_value());
    EXPECT_EQ(shortFile.error(), VK_ERROR_INITIALIZATION_FAILED);

    const auto missing =
        Texture::loadFromFile(deviceCtx(), commandPool(), scratchFile("does_not_exist.dds"), TextureColorSpace::Data);
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error(), VK_ERROR_INITIALIZATION_FAILED);

    std::filesystem::remove(disguised);
    std::filesystem::remove(truncated);
}

// Negative controls of the upload API itself: the chain is validated before any Vulkan
// object is created, so none of these may produce an image or a validation message.
TEST_F(VulkanFixture, HostImageUpload_RefusesAnInconsistentMipChain) {
    const auto& ctx = deviceCtx();
    auto dds = DdsImage::parse(readFile(fixture("bc7_checker16.dds")));
    ASSERT_TRUE(dds.has_value()) << harmonia::core::ddsErrorMessage(dds.error());
    ASSERT_EQ(dds->mipLevels(), kLevels);
    const std::vector<HostImageMipLevel> good = mipLevelsOf(*dds);
    ASSERT_EQ(good.size(), kLevels);

    const auto upload = [&](VkFormat format, const std::vector<HostImageMipLevel>& levels) {
        return harmonia::core::uploadHostImage(ctx,
                                               commandPool(),
                                               harmonia::core::HostImageUploadDesc{
                                                   .format = format,
                                                   .pixels = nullptr,
                                                   .extent = VkExtent2D{kSize, kSize},
                                                   .finalStages = kStages,
                                                   .finalAccess = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                                                   .name = "test.dds.negativeControl",
                                                   .sampler = linearSampler(),
                                                   .mips = levels,
                                               });
    };

    // Positive control: the untouched chain is accepted and stays at exactly five levels.
    {
        auto accepted = upload(VK_FORMAT_BC7_UNORM_BLOCK, good);
        ASSERT_TRUE(accepted.has_value()) << "VkResult " << static_cast<int>(accepted.error());
        EXPECT_EQ(accepted->image.mipLevels(), kLevels);
        EXPECT_EQ(accepted->image.format(), VK_FORMAT_BC7_UNORM_BLOCK);
        EXPECT_NE(accepted->sampler.get(), VK_NULL_HANDLE);
    }

    // A level range one block short of its extent.
    std::vector<HostImageMipLevel> shortLevel = good;
    shortLevel[1].byteLength -= 16U;
    auto rejected = upload(VK_FORMAT_BC7_UNORM_BLOCK, shortLevel);
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(rejected.error(), VK_ERROR_INITIALIZATION_FAILED);

    // A level whose extent is not the one its level number implies.
    std::vector<HostImageMipLevel> wrongExtent = good;
    wrongExtent[2].width = 5U;
    rejected = upload(VK_FORMAT_BC7_UNORM_BLOCK, wrongExtent);
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(rejected.error(), VK_ERROR_INITIALIZATION_FAILED);

    // A null host pointer.
    std::vector<HostImageMipLevel> nullData = good;
    nullData[3].data = nullptr;
    rejected = upload(VK_FORMAT_BC7_UNORM_BLOCK, nullData);
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(rejected.error(), VK_ERROR_INITIALIZATION_FAILED);

    // One level more than a 16x16 image has.
    std::vector<HostImageMipLevel> tooMany = good;
    tooMany.push_back(good.back());
    rejected = upload(VK_FORMAT_BC7_UNORM_BLOCK, tooMany);
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(rejected.error(), VK_ERROR_INITIALIZATION_FAILED);

    // A format whose texel footprint the upload does not know.
    rejected = upload(VK_FORMAT_R8_UNORM, good);
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(rejected.error(), VK_ERROR_INITIALIZATION_FAILED);
}

// A precomputed chain is never extended: one supplied level stays one level, where the
// single-level path would have generated four more.
TEST_F(HostImageUploadDds, APrecomputedChainIsNeverExtended) {
    std::array<Rgba, kSize * kSize> pixels{};
    for (std::uint32_t index = 0U; index < pixels.size(); ++index) {
        pixels[index] = Rgba{static_cast<std::uint8_t>(index),
                             static_cast<std::uint8_t>(index / 7U),
                             static_cast<std::uint8_t>(255U - index),
                             0xFF};
    }
    const HostImageMipLevel single{
        .data = pixels.data(),
        .byteLength = sizeof(pixels),
        .width = kSize,
        .height = kSize,
    };
    auto uploaded = harmonia::core::uploadHostImage(deviceCtx(),
                                                    commandPool(),
                                                    harmonia::core::HostImageUploadDesc{
                                                        .format = VK_FORMAT_R8G8B8A8_UNORM,
                                                        .pixels = nullptr,
                                                        .extent = VkExtent2D{kSize, kSize},
                                                        .finalStages = kStages,
                                                        .finalAccess = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                                                        .name = "test.dds.singleLevelChain",
                                                        .sampler = linearSampler(),
                                                        .mips = std::span<const HostImageMipLevel>{&single, 1U},
                                                    });
    ASSERT_TRUE(uploaded.has_value()) << "VkResult " << static_cast<int>(uploaded.error());
    EXPECT_EQ(uploaded->image.mipLevels(), 1U);

    LevelReader reader;
    auto created = LevelReader::create(deviceCtx());
    ASSERT_TRUE(created.has_value()) << created.error();
    reader = std::move(*created);
    const std::vector<sm::float4> texels =
        reader.read(deviceCtx(), commandPool(), uploaded->image.view(), 0U, kSize, kSize);
    const std::vector<Rgba> expected{pixels.begin(), pixels.end()};
    EXPECT_EQ(compare(texels, expected, 4).max, 0.0F);
}

// The single-level RGBA8 path is unchanged: level 0 is the uploaded data, and the chain
// below it is still generated by the GPU (five levels for 16x16).
TEST_F(HostImageUploadDds, TheSingleLevelRgba8PathIsUnchanged) {
    const std::vector<Rgba> source = readSource(fixture("bc7_checker16.tga"));
    ASSERT_EQ(source.size(), kSize * kSize);
    auto texture =
        Texture::create(deviceCtx(), commandPool(), std::as_bytes(std::span<const Rgba>{source}), kSize, kSize);
    ASSERT_TRUE(texture.has_value()) << "VkResult " << static_cast<int>(texture.error());
    EXPECT_EQ(texture->image().format(), VK_FORMAT_R8G8B8A8_UNORM);
    EXPECT_EQ(texture->mipLevels(), kLevels);
    EXPECT_EQ(compare(readLevel(*texture, 0U), source, 4).max, 0.0F);

    // The generated chain is readable at every level and finite.
    for (std::uint32_t level = 1U; level < kLevels; ++level) {
        const std::vector<sm::float4> texels = readLevel(*texture, level);
        EXPECT_EQ(texels.size(), std::max(1U, kSize >> level) * std::max(1U, kSize >> level));
        for (const sm::float4& texel : texels) {
            EXPECT_TRUE(std::isfinite(texel.x) && std::isfinite(texel.y) && std::isfinite(texel.z)) << level;
        }
    }
}
