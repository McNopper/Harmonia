#include "harmonia/pipeline/GlarePass.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <utility>

#include "harmonia/core/Barrier.hpp"
#include "harmonia/core/Logger.hpp"
#include "harmonia/core/PipelineBinaryCache.hpp"
#include "harmonia/core/ShaderModule.hpp"
#include "harmonia/pipeline/PassContext.hpp"

namespace harmonia::pipeline {

namespace {

struct alignas(4) GlarePushConstants {
    float sourceTexel[2]{1.0F, 1.0F}; ///< 1 / size of the source (the coarser copy for the upsample)
    float targetTexel[2]{1.0F, 1.0F}; ///< 1 / size of the target
    float threshold = 1.0F;           ///< the prefilter: luminance above which a texel is scattered
    float weight = 0.0F;              ///< the upsample: share of the coarser copy
};
static_assert(sizeof(GlarePushConstants) == 24);

/// One more barrier on every boundary between two dispatches: the image a
/// dispatch wrote is read by the next.
void writtenToReadable(VkCommandBuffer cmd, VkImage image) {
    const std::array barrier{core::imageBarrier(image,
                                                VK_IMAGE_LAYOUT_GENERAL,
                                                VK_IMAGE_LAYOUT_GENERAL,
                                                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                                VK_ACCESS_2_SHADER_WRITE_BIT,
                                                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                                VK_ACCESS_2_SHADER_READ_BIT)};
    core::pipelineBarrier(cmd, barrier);
}

/// Half an extent, each dimension rounded up and never below one texel.
[[nodiscard]] VkExtent2D halfOf(VkExtent2D extent) noexcept {
    return VkExtent2D{(extent.width + 1U) / 2U, (extent.height + 1U) / 2U};
}

} // namespace

std::expected<GlarePass, VkResult>
GlarePass::create(const DeviceContext& ctx, VkExtent2D extent, const std::filesystem::path& shaderDirectory) {
    return create(ctx, extent, shaderDirectory, GlarePass::Config{});
}

std::expected<GlarePass, VkResult> GlarePass::create(const DeviceContext& ctx,
                                                     VkExtent2D extent,
                                                     const std::filesystem::path& shaderDirectory,
                                                     const Config& config) {
    if (!ctx.isValid()) {
        return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
    }
    if (!(config.threshold > 0.0F) || !(config.levelWeight >= 0.0F && config.levelWeight <= 1.0F)) {
        core::Logger::error("GlarePass: the threshold must be above 0 and the level weight within 0..1");
        return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
    }

    // Three storage images: the source, the coarser copy (the upsample only)
    // and the target, all in GENERAL. An unused binding is a null descriptor.
    const std::array bindings{
        VkDescriptorSetLayoutBinding{
            .binding = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            .pImmutableSamplers = nullptr,
        },
        VkDescriptorSetLayoutBinding{
            .binding = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            .pImmutableSamplers = nullptr,
        },
        VkDescriptorSetLayoutBinding{
            .binding = 2,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            .pImmutableSamplers = nullptr,
        },
    };
    const VkDescriptorSetLayoutCreateInfo setLayoutInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .pNext = nullptr,
        .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT,
        .bindingCount = static_cast<std::uint32_t>(bindings.size()),
        .pBindings = bindings.data(),
    };

    GlarePass pass;
    pass.m_ctx = &ctx;
    pass.m_config = config;
    VkDescriptorSetLayout setLayout{};
    if (const VkResult result = vkCreateDescriptorSetLayout(ctx.device, &setLayoutInfo, nullptr, &setLayout);
        result != VK_SUCCESS) {
        return std::unexpected(result);
    }
    pass.m_setLayout = core::UniqueDescriptorSetLayout{ctx.device, setLayout};

    const VkPushConstantRange pushRange{
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset = 0,
        .size = sizeof(GlarePushConstants),
    };
    const VkPipelineLayoutCreateInfo pipelineLayoutInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .setLayoutCount = 1,
        .pSetLayouts = pass.m_setLayout.ptr(),
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pushRange,
    };
    VkPipelineLayout pipelineLayout{};
    if (const VkResult result = vkCreatePipelineLayout(ctx.device, &pipelineLayoutInfo, nullptr, &pipelineLayout);
        result != VK_SUCCESS) {
        return std::unexpected(result);
    }
    pass.m_pipelineLayout = core::UniquePipelineLayout{ctx.device, pipelineLayout};

    if (!pass.createPipelines(shaderDirectory)) {
        return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
    }
    if (!pass.createImages(extent)) {
        return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
    }

    ctx.setDebugName(VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, pass.m_setLayout.get(), "harmonia.glare.setLayout");
    ctx.setDebugName(VK_OBJECT_TYPE_PIPELINE_LAYOUT, pass.m_pipelineLayout.get(), "harmonia.glare.pipelineLayout");
    return pass;
}

bool GlarePass::createPipelines(const std::filesystem::path& shaderDirectory) {
    // Each module is the entry of its name compiled on its own (see
    // HARMONIA_ENTRY_SHADERS), and Slang exports a selected entry as "main".
    const auto build = [this, &shaderDirectory](const char* file, const char* cacheName, core::UniquePipeline& out) {
        auto module = core::createShaderModule(m_ctx->device, shaderDirectory / file);
        if (!module) {
            core::Logger::error("GlarePass: cannot load shader {}", file);
            return false;
        }
        const VkComputePipelineCreateInfo pipelineInfo{
            .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .stage =
                {
                    .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                    .pNext = nullptr,
                    .flags = 0,
                    .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                    .module = *module,
                    .pName = "main",
                    .pSpecializationInfo = nullptr,
                },
            .layout = m_pipelineLayout,
            .basePipelineHandle = VK_NULL_HANDLE,
            .basePipelineIndex = 0,
        };
        auto pipeline = core::PipelineBinaryCache::computePipeline(*m_ctx, pipelineInfo, cacheName);
        vkDestroyShaderModule(m_ctx->device, *module, nullptr);
        if (!pipeline) {
            core::Logger::error(
                "GlarePass: pipeline creation failed for {}: VkResult {}", file, static_cast<int>(pipeline.error()));
            return false;
        }
        out = core::UniquePipeline{m_ctx->device, *pipeline};
        m_ctx->setDebugName(VK_OBJECT_TYPE_PIPELINE, out.get(), cacheName);
        return true;
    };
    return build("glare.prefilterMain.spv", "harmonia.glare.prefilter", m_pipelinePrefilter) &&
           build("glare.downsampleMain.spv", "harmonia.glare.downsample", m_pipelineDownsample) &&
           build("glare.upsampleMain.spv", "harmonia.glare.upsample", m_pipelineUpsample);
}

bool GlarePass::createImages(VkExtent2D extent) {
    destroyImages();
    m_extent = extent;
    // A level exists while both of its dimensions are at least 2; a frame too small
    // for one has no glare. The levels hold the frame's light above the threshold:
    // the frame's format, so that they carry its whole range. The result can be
    // read back.
    constexpr VkFormat kLevelFormat = VK_FORMAT_R32G32B32A32_SFLOAT;
    VkExtent2D level = halfOf(extent);
    while (level.width >= 2U && level.height >= 2U && m_levelCount < kMaxLevels) {
        const std::uint32_t index = m_levelCount;
        for (auto* chain : {&m_downsampled, &m_upsampled}) {
            const bool isResult = index == 0U && chain == &m_upsampled;
            const VkImageUsageFlags usage =
                VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                (isResult ? static_cast<VkImageUsageFlags>(VK_IMAGE_USAGE_TRANSFER_SRC_BIT) : VkImageUsageFlags{0});
            auto image = core::Image::create(*m_ctx,
                                             level,
                                             kLevelFormat,
                                             usage,
                                             VK_IMAGE_ASPECT_COLOR_BIT,
                                             isResult ? "harmonia.glare.result" : "harmonia.glare.level");
            if (!image) {
                core::Logger::error("GlarePass: image creation failed: VkResult {}", static_cast<int>(image.error()));
                destroyImages();
                return false;
            }
            (*chain)[index] = std::move(*image);
        }
        m_levelExtents[index] = level;
        ++m_levelCount;
        level = halfOf(level);
    }
    return true;
}

void GlarePass::destroyImages() noexcept {
    for (std::uint32_t index = 0; index < kMaxLevels; ++index) {
        m_downsampled[index] = {};
        m_upsampled[index] = {};
    }
    m_levelCount = 0U;
}

void GlarePass::onResize(VkExtent2D extent) noexcept {
    if (m_ctx == nullptr || (extent.width == m_extent.width && extent.height == m_extent.height)) {
        return;
    }
    static_cast<void>(createImages(extent));
}

void GlarePass::dispatch(VkCommandBuffer cmd, const Step& step) const noexcept {
    const std::array infos{
        VkDescriptorImageInfo{
            .sampler = VK_NULL_HANDLE, .imageView = step.source, .imageLayout = VK_IMAGE_LAYOUT_GENERAL},
        VkDescriptorImageInfo{
            .sampler = VK_NULL_HANDLE, .imageView = step.coarse, .imageLayout = VK_IMAGE_LAYOUT_GENERAL},
        VkDescriptorImageInfo{
            .sampler = VK_NULL_HANDLE, .imageView = step.target, .imageLayout = VK_IMAGE_LAYOUT_GENERAL},
    };
    std::array<VkWriteDescriptorSet, 3> writes{};
    for (std::uint32_t binding = 0U; binding < writes.size(); ++binding) {
        writes[binding] = VkWriteDescriptorSet{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .pNext = nullptr,
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = binding,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .pImageInfo = &infos[binding],
            .pBufferInfo = nullptr,
            .pTexelBufferView = nullptr,
        };
    }
    vkCmdPushDescriptorSet(cmd,
                           VK_PIPELINE_BIND_POINT_COMPUTE,
                           m_pipelineLayout,
                           0U,
                           static_cast<std::uint32_t>(writes.size()),
                           writes.data());

    const GlarePushConstants push{
        .sourceTexel = {1.0F / static_cast<float>(step.sourceExtent.width),
                        1.0F / static_cast<float>(step.sourceExtent.height)},
        .targetTexel = {1.0F / static_cast<float>(step.targetExtent.width),
                        1.0F / static_cast<float>(step.targetExtent.height)},
        .threshold = step.threshold,
        .weight = step.weight,
    };
    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0U, sizeof(push), &push);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, step.pipeline);
    vkCmdDispatch(cmd, dispatchGroups1D(step.targetExtent.width), dispatchGroups1D(step.targetExtent.height), 1U);
}

void GlarePass::record(const PassContext& ctx) noexcept {
    // The image the tone mapper maps: the denoiser's output when the denoiser
    // ran, else the accumulated frame.
    const core::Image* source = ctx.denoised != nullptr ? ctx.denoised : ctx.hdrBuffer;
    if (source == nullptr || ctx.cmd == VK_NULL_HANDLE) {
        return;
    }
    if (ctx.extent.width != m_extent.width || ctx.extent.height != m_extent.height) {
        onResize(ctx.extent);
    }
    // The stage list names no writer of that image for this stage: wait for
    // everything recorded before it.
    record(ctx.cmd,
           *source,
           VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
           VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT);
}

void GlarePass::record(VkCommandBuffer cmd,
                       const core::Image& source,
                       VkPipelineStageFlags2 srcStage,
                       VkAccessFlags2 srcAccess) noexcept {
    if (m_pipelinePrefilter == VK_NULL_HANDLE || m_levelCount == 0U || cmd == VK_NULL_HANDLE) {
        return;
    }

    // Every level is fully written before it is read in this pass, so its
    // previous content is discarded: that was written by the previous frame's
    // dispatches, and the result was read by its tone mapper.
    std::array<VkImageMemoryBarrier2, 2U * kMaxLevels + 1U> barriers{};
    std::size_t count = 0U;
    barriers[count++] = core::imageBarrier(source.handle(),
                                           VK_IMAGE_LAYOUT_GENERAL,
                                           VK_IMAGE_LAYOUT_GENERAL,
                                           srcStage,
                                           srcAccess,
                                           VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                           VK_ACCESS_2_SHADER_READ_BIT);
    for (std::uint32_t index = 0U; index < m_levelCount; ++index) {
        for (const auto* chain : {&m_downsampled, &m_upsampled}) {
            barriers[count++] =
                core::imageBarrier((*chain)[index].handle(),
                                   VK_IMAGE_LAYOUT_UNDEFINED,
                                   VK_IMAGE_LAYOUT_GENERAL,
                                   VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                   VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_SHADER_READ_BIT,
                                   VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                   VK_ACCESS_2_SHADER_WRITE_BIT);
        }
    }
    core::pipelineBarrier(cmd, std::span<const VkImageMemoryBarrier2>{barriers.data(), count});

    const VkExtent2D frameExtent = source.extent();
    dispatch(cmd,
             {.pipeline = m_pipelinePrefilter,
              .source = source.view(),
              .target = m_downsampled[0].view(),
              .sourceExtent = frameExtent,
              .targetExtent = m_levelExtents[0],
              .threshold = m_config.threshold});
    writtenToReadable(cmd, m_downsampled[0].handle());

    for (std::uint32_t index = 1U; index < m_levelCount; ++index) {
        dispatch(cmd,
                 {.pipeline = m_pipelineDownsample,
                  .source = m_downsampled[index - 1U].view(),
                  .target = m_downsampled[index].view(),
                  .sourceExtent = m_levelExtents[index - 1U],
                  .targetExtent = m_levelExtents[index]});
        writtenToReadable(cmd, m_downsampled[index].handle());
    }

    // The coarsest level is copied; every level above adds a tent-filtered
    // copy of the coarser one.
    const std::uint32_t last = m_levelCount - 1U;
    dispatch(cmd,
             {.pipeline = m_pipelineUpsample,
              .source = m_downsampled[last].view(),
              .target = m_upsampled[last].view(),
              .sourceExtent = m_levelExtents[last],
              .targetExtent = m_levelExtents[last]});
    writtenToReadable(cmd, m_upsampled[last].handle());
    for (std::uint32_t index = last; index-- > 0U;) {
        dispatch(cmd,
                 {.pipeline = m_pipelineUpsample,
                  .source = m_downsampled[index].view(),
                  .coarse = m_upsampled[index + 1U].view(),
                  .target = m_upsampled[index].view(),
                  .sourceExtent = m_levelExtents[index + 1U],
                  .targetExtent = m_levelExtents[index],
                  .weight = m_config.levelWeight});
        writtenToReadable(cmd, m_upsampled[index].handle());
    }

    const std::array final{
        core::imageBarrier(m_upsampled[0].handle(),
                           VK_IMAGE_LAYOUT_GENERAL,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                           VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                           VK_ACCESS_2_SHADER_WRITE_BIT,
                           VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                           VK_ACCESS_2_SHADER_READ_BIT),
    };
    core::pipelineBarrier(cmd, final);
}

} // namespace harmonia::pipeline
