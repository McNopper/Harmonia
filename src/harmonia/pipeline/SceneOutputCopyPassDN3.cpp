// DN3 (converging blend, Denisova et al. I3D 2024) pass machinery, split from
// SceneOutputCopyPassDenoise.cpp to hold the architecture size gate.

#include <volk/volk.h>

#include <array>
#include <cstdint>
#include <utility>

#include "harmonia/core/Barrier.hpp"
#include "harmonia/core/Logger.hpp"
#include "harmonia/pipeline/SceneOutputCopyPass.hpp"
#include "harmonia/pipeline/SceneOutputCopyPassDetail.hpp"

namespace harmonia::pipeline {

void SceneOutputCopyPass::recordTemporalHistoryPass(VkCommandBuffer cmd,
                                                    const core::Image& denoised,
                                                    const core::Image& hdrOriginal,
                                                    bool useGradient,
                                                    bool hasMotionVectors,
                                                    const DispatchGrid& grid,
                                                    const DenoiseGuides& guides,
                                                    std::uint32_t sampleCount,
                                                    std::uint32_t prevSampleCount,
                                                    bool convergingActive) noexcept {
    // Unpack the parameter objects into the historical local names (read-only below).
    // hasGradientVariance is deliberately not unpacked: the temporal pass writes, never
    // reads, the variance.
    const std::uint32_t iterations = grid.iterations;
    const std::uint32_t groupsX = grid.groupsX;
    const std::uint32_t groupsY = grid.groupsY;
    const VkImageView normalGuideView = guides.normalView;
    const VkImageView depthGuideView = guides.depthView;
    const bool hasNormalGuide = guides.hasNormal;
    const bool hasDepthGuide = guides.hasDepth;
    const VkImageView motionVecView = guides.motionVecView;
    const VkImageView gradientView = guides.gradientView;
    const VkImageView prevGradientView = guides.prevGradientView;
    const VkDescriptorImageInfo denoisedInfo{
        .sampler = VK_NULL_HANDLE,
        .imageView = denoised.view(),
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
    };
    const VkDescriptorImageInfo normalGuideInfo{
        .sampler = m_guideSampler,
        .imageView = normalGuideView,
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
    };
    const VkDescriptorImageInfo depthGuideInfo{
        .sampler = m_guideSampler,
        .imageView = depthGuideView,
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
    };
    const VkDescriptorImageInfo historyInfo{
        .sampler = VK_NULL_HANDLE,
        .imageView = m_historyImage.view(),
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
    };
    const VkDescriptorImageInfo motionVecHistInfo{
        .sampler = VK_NULL_HANDLE,
        .imageView = motionVecView,
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
    };
    const VkDescriptorImageInfo gradientHistInfo{
        .sampler = VK_NULL_HANDLE,
        .imageView = gradientView,
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
    };
    const VkDescriptorImageInfo prevGradientHistInfo{
        .sampler = VK_NULL_HANDLE,
        .imageView = prevGradientView,
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
    };
    // DN3-M2 (paper Eq. 2 + Eq. 14): original accumulated input n and the
    // perturbed-chain output D' (read only when convergingMode is pushed).
    const VkDescriptorImageInfo originalInfo{
        .sampler = VK_NULL_HANDLE,
        .imageView = hdrOriginal.view(),
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
    };
    const VkDescriptorImageInfo perturbedInfo{
        .sampler = VK_NULL_HANDLE,
        .imageView = m_perturbedOutput.isValid() ? m_perturbedOutput.view() : m_workImage.view(),
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
    };
    const std::array historyWrites{
        VkWriteDescriptorSet{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .pNext = nullptr,
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = 0,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .pImageInfo = &denoisedInfo,
            .pBufferInfo = nullptr,
            .pTexelBufferView = nullptr,
        },
        VkWriteDescriptorSet{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .pNext = nullptr,
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = 1,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .pImageInfo = &denoisedInfo,
            .pBufferInfo = nullptr,
            .pTexelBufferView = nullptr,
        },
        VkWriteDescriptorSet{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .pNext = nullptr,
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = 2,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &normalGuideInfo,
            .pBufferInfo = nullptr,
            .pTexelBufferView = nullptr,
        },
        VkWriteDescriptorSet{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .pNext = nullptr,
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = 3,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &depthGuideInfo,
            .pBufferInfo = nullptr,
            .pTexelBufferView = nullptr,
        },
        VkWriteDescriptorSet{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .pNext = nullptr,
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = 4,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .pImageInfo = &historyInfo,
            .pBufferInfo = nullptr,
            .pTexelBufferView = nullptr,
        },
        VkWriteDescriptorSet{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .pNext = nullptr,
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = 5,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .pImageInfo = &motionVecHistInfo,
            .pBufferInfo = nullptr,
            .pTexelBufferView = nullptr,
        },
        VkWriteDescriptorSet{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .pNext = nullptr,
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = 6,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .pImageInfo = &gradientHistInfo,
            .pBufferInfo = nullptr,
            .pTexelBufferView = nullptr,
        },
        VkWriteDescriptorSet{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .pNext = nullptr,
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = 7,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .pImageInfo = &prevGradientHistInfo,
            .pBufferInfo = nullptr,
            .pTexelBufferView = nullptr,
        },
        VkWriteDescriptorSet{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .pNext = nullptr,
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = 8,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .pImageInfo = &originalInfo,
            .pBufferInfo = nullptr,
            .pTexelBufferView = nullptr,
        },
        VkWriteDescriptorSet{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .pNext = nullptr,
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = 9,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .pImageInfo = &perturbedInfo,
            .pBufferInfo = nullptr,
            .pTexelBufferView = nullptr,
        },
    };

    const DenoiserPushConstants push{
        .strength = m_settings.strength,
        .historyBlend = m_settings.historyBlend,
        .hasNormalGuide = hasNormalGuide ? 1U : 0U,
        .hasDepthGuide = hasDepthGuide ? 1U : 0U,
        .applyHistory = 1U,
        .historyFirstUse = m_historyFirstUse ? 1U : 0U,
        .iterations = iterations,
        .passIndex = iterations,
        .hasMotionVectors = hasMotionVectors ? 1U : 0U,
        .computeGradient = useGradient ? 1U : 0U, // temporal pass writes gradient + variance
        .gradientFilterMode = 0U,
        .gradientFilterPass = 0U,
        // DN3-M2: the temporal invocation reads the variance (binding 6) for the
        // converging blend's error estimate when active.
        .hasGradientVariance = (useGradient && convergingActive) ? 1U : 0U,
        .gradientAlpha = m_settings.gradientAlpha,
        .convergingMode = convergingActive ? 1U : 0U,
        .perturbedMode = 0U,
        .sampleCount = sampleCount,
        .prevSampleCount = prevSampleCount,
    };
    vkCmdPushDescriptorSet(cmd,
                           VK_PIPELINE_BIND_POINT_COMPUTE,
                           m_pipelineLayout,
                           0U,
                           static_cast<std::uint32_t>(historyWrites.size()),
                           historyWrites.data());
    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0U, sizeof(push), &push);
    vkCmdDispatch(cmd, groupsX, groupsY, 1U);
}

void SceneOutputCopyPass::recordGradientBlur(VkCommandBuffer cmd,
                                             VkExtent2D extent,
                                             std::uint32_t iterations,
                                             std::uint32_t groupsX,
                                             std::uint32_t groupsY) noexcept {
    // ── A-SVGF gradient à-trous propagation + prev-frame carry ──────────────────
    // After the temporal pass has written this frame's per-pixel gradient into
    // m_gradientImage, spread it spatially with a few à-trous-spaced Gaussian
    // passes (ping-ponging m_gradientImage ↔ m_prevGradientImage; an even pass
    // count returns the result to m_gradientImage), then copy it into
    // m_prevGradientImage so next frame's temporal smoothing and variance guiding
    // read a defined, consistent value. On first use the temporal pass only seeds
    // the gradient to zero, so the blur is skipped and only the carry copy runs.
    if (!m_historyFirstUse) {
        constexpr std::uint32_t kGradientFilterPasses = 2U; // even → final result lands in m_gradientImage
        for (std::uint32_t gp = 0U; gp < kGradientFilterPasses; ++gp) {
            const bool srcIsGradient = (gp % 2U) == 0U;
            const VkImageView gradSrcView = srcIsGradient ? m_gradientImage.view() : m_prevGradientImage.view();
            const VkImageView gradDstView = srcIsGradient ? m_prevGradientImage.view() : m_gradientImage.view();
            const VkImage gradSrcImage = srcIsGradient ? m_gradientImage.handle() : m_prevGradientImage.handle();
            const VkImage gradDstImage = srcIsGradient ? m_prevGradientImage.handle() : m_gradientImage.handle();

            const std::array gradPassBarriers{
                core::imageBarrier(gradSrcImage,
                                   VK_IMAGE_LAYOUT_GENERAL,
                                   VK_IMAGE_LAYOUT_GENERAL,
                                   VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                   VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
                                   VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                   VK_ACCESS_2_SHADER_READ_BIT),
                core::imageBarrier(gradDstImage,
                                   VK_IMAGE_LAYOUT_GENERAL,
                                   VK_IMAGE_LAYOUT_GENERAL,
                                   VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                   VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
                                   VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                   VK_ACCESS_2_SHADER_WRITE_BIT),
            };
            core::pipelineBarrier(cmd, gradPassBarriers);

            // binding 6 = destination (gGradientVariance, written);
            // binding 7 = source (gPrevGradientVariance, read neighbourhood).
            const VkDescriptorImageInfo gradDstInfo{
                .sampler = VK_NULL_HANDLE,
                .imageView = gradDstView,
                .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
            };
            const VkDescriptorImageInfo gradSrcInfo{
                .sampler = VK_NULL_HANDLE,
                .imageView = gradSrcView,
                .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
            };
            const std::array gradWrites{
                VkWriteDescriptorSet{
                    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                    .pNext = nullptr,
                    .dstSet = VK_NULL_HANDLE,
                    .dstBinding = 6,
                    .dstArrayElement = 0,
                    .descriptorCount = 1,
                    .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                    .pImageInfo = &gradDstInfo,
                    .pBufferInfo = nullptr,
                    .pTexelBufferView = nullptr,
                },
                VkWriteDescriptorSet{
                    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                    .pNext = nullptr,
                    .dstSet = VK_NULL_HANDLE,
                    .dstBinding = 7,
                    .dstArrayElement = 0,
                    .descriptorCount = 1,
                    .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                    .pImageInfo = &gradSrcInfo,
                    .pBufferInfo = nullptr,
                    .pTexelBufferView = nullptr,
                },
            };

            DenoiserPushConstants gradPush{};
            gradPush.strength = m_settings.strength;
            gradPush.historyBlend = m_settings.historyBlend;
            gradPush.iterations = iterations;
            gradPush.passIndex = iterations;
            gradPush.gradientFilterMode = 1U;
            gradPush.gradientFilterPass = gp;
            gradPush.gradientAlpha = m_settings.gradientAlpha;

            vkCmdPushDescriptorSet(cmd,
                                   VK_PIPELINE_BIND_POINT_COMPUTE,
                                   m_pipelineLayout,
                                   0U,
                                   static_cast<std::uint32_t>(gradWrites.size()),
                                   gradWrites.data());
            vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0U, sizeof(gradPush), &gradPush);
            vkCmdDispatch(cmd, groupsX, groupsY, 1U);
        }
    }

    // Carry the (possibly blurred) gradient into the previous-frame buffer.
    copyImageRoundTrip(cmd,
                       m_gradientImage.handle(),
                       m_prevGradientImage.handle(),
                       extent,
                       VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
}

void SceneOutputCopyPass::recordPerturbedChain(VkCommandBuffer cmd,
                                               const core::Image& hdrOriginal,
                                               const DispatchGrid& grid,
                                               const DenoiseGuides& guides) noexcept {
    // DN3-M2 perturbed chain (paper Eq. 13 probe, K = 1): the same atrous
    // sequence applied to n + eps*b. Pass 0 reads the original input and adds
    // the static hash draw inline; later passes ping-pong the two scratches.
    // Final D' lands in m_perturbedOutput (round-trip copied when the pass
    // count is even).
    const std::uint32_t iterations = grid.iterations;
    const std::uint32_t groupsX = grid.groupsX;
    const std::uint32_t groupsY = grid.groupsY;
    VkImageView srcView = hdrOriginal.view();
    VkImage srcImage = hdrOriginal.handle();

    // First use: the scratch is created UNDEFINED - transition once to GENERAL.
    if (m_perturbedFirstUse) {
        const std::array firstUseBarrier{
            core::imageBarrier(m_perturbedOutput.handle(),
                               VK_IMAGE_LAYOUT_UNDEFINED,
                               VK_IMAGE_LAYOUT_GENERAL,
                               VK_PIPELINE_STAGE_2_NONE,
                               0U,
                               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                               VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT),
        };
        core::pipelineBarrier(cmd, firstUseBarrier);
        m_perturbedFirstUse = false;
    }

    for (std::uint32_t passIndex = 0U; passIndex < iterations; ++passIndex) {
        const bool writeToPerturbed = ((passIndex & 1U) == 0U);
        const VkImageView dstView = writeToPerturbed ? m_perturbedOutput.view() : m_workImage.view();
        const VkImage dstImage = writeToPerturbed ? m_perturbedOutput.handle() : m_workImage.handle();

        const VkDescriptorImageInfo srcInfo{
            .sampler = VK_NULL_HANDLE,
            .imageView = srcView,
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        };
        const VkDescriptorImageInfo dstInfo{
            .sampler = VK_NULL_HANDLE,
            .imageView = dstView,
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        };
        const VkDescriptorImageInfo normalGuideInfo{
            .sampler = m_guideSampler,
            .imageView = guides.normalView,
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        };
        const VkDescriptorImageInfo depthGuideInfo{
            .sampler = m_guideSampler,
            .imageView = guides.depthView,
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        };
        const VkDescriptorImageInfo gradientInfo{
            .sampler = VK_NULL_HANDLE,
            .imageView = guides.gradientView,
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        };
        const VkDescriptorImageInfo prevGradientInfo{
            .sampler = VK_NULL_HANDLE,
            .imageView = guides.prevGradientView,
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        };
        // DN3-M2: the perturbed dispatch uses the same module - the full binding
        // set must be pushed (history/motion for the static-use tail, 8/9 for the
        // SURE read in the temporal invocation).
        const VkDescriptorImageInfo historyProbeInfo{
            .sampler = VK_NULL_HANDLE,
            .imageView = m_historyImage.view(),
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        };
        const VkDescriptorImageInfo motionProbeInfo{
            .sampler = VK_NULL_HANDLE,
            .imageView = guides.motionVecView,
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        };
        const VkDescriptorImageInfo chainOriginalInfo{
            .sampler = VK_NULL_HANDLE,
            .imageView = hdrOriginal.view(),
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        };
        const VkDescriptorImageInfo chainPerturbedInfo{
            .sampler = VK_NULL_HANDLE,
            .imageView = m_perturbedOutput.view(),
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        };
        const std::array writes{
            VkWriteDescriptorSet{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .pNext = nullptr,
                .dstSet = VK_NULL_HANDLE,
                .dstBinding = 0,
                .dstArrayElement = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                .pImageInfo = &srcInfo,
                .pBufferInfo = nullptr,
                .pTexelBufferView = nullptr,
            },
            VkWriteDescriptorSet{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .pNext = nullptr,
                .dstSet = VK_NULL_HANDLE,
                .dstBinding = 1,
                .dstArrayElement = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                .pImageInfo = &dstInfo,
                .pBufferInfo = nullptr,
                .pTexelBufferView = nullptr,
            },
            VkWriteDescriptorSet{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .pNext = nullptr,
                .dstSet = VK_NULL_HANDLE,
                .dstBinding = 2,
                .dstArrayElement = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .pImageInfo = &normalGuideInfo,
                .pBufferInfo = nullptr,
                .pTexelBufferView = nullptr,
            },
            VkWriteDescriptorSet{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .pNext = nullptr,
                .dstSet = VK_NULL_HANDLE,
                .dstBinding = 3,
                .dstArrayElement = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .pImageInfo = &depthGuideInfo,
                .pBufferInfo = nullptr,
                .pTexelBufferView = nullptr,
            },
            VkWriteDescriptorSet{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .pNext = nullptr,
                .dstSet = VK_NULL_HANDLE,
                .dstBinding = 6,
                .dstArrayElement = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                .pImageInfo = &gradientInfo,
                .pBufferInfo = nullptr,
                .pTexelBufferView = nullptr,
            },
            VkWriteDescriptorSet{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .pNext = nullptr,
                .dstSet = VK_NULL_HANDLE,
                .dstBinding = 7,
                .dstArrayElement = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                .pImageInfo = &prevGradientInfo,
                .pBufferInfo = nullptr,
                .pTexelBufferView = nullptr,
            },
            VkWriteDescriptorSet{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .pNext = nullptr,
                .dstSet = VK_NULL_HANDLE,
                .dstBinding = 4,
                .dstArrayElement = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                .pImageInfo = &historyProbeInfo,
                .pBufferInfo = nullptr,
                .pTexelBufferView = nullptr,
            },
            VkWriteDescriptorSet{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .pNext = nullptr,
                .dstSet = VK_NULL_HANDLE,
                .dstBinding = 5,
                .dstArrayElement = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                .pImageInfo = &motionProbeInfo,
                .pBufferInfo = nullptr,
                .pTexelBufferView = nullptr,
            },
            VkWriteDescriptorSet{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .pNext = nullptr,
                .dstSet = VK_NULL_HANDLE,
                .dstBinding = 8,
                .dstArrayElement = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                .pImageInfo = &chainOriginalInfo,
                .pBufferInfo = nullptr,
                .pTexelBufferView = nullptr,
            },
            VkWriteDescriptorSet{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .pNext = nullptr,
                .dstSet = VK_NULL_HANDLE,
                .dstBinding = 9,
                .dstArrayElement = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                .pImageInfo = &chainPerturbedInfo,
                .pBufferInfo = nullptr,
                .pTexelBufferView = nullptr,
            },
        };

        const DenoiserPushConstants push{
            .strength = m_settings.strength,
            .historyBlend = m_settings.historyBlend,
            .hasNormalGuide = guides.hasNormal ? 1U : 0U,
            .hasDepthGuide = guides.hasDepth ? 1U : 0U,
            .applyHistory = 0U,
            .historyFirstUse = 0U,
            .iterations = iterations,
            .passIndex = passIndex,
            .hasMotionVectors = 0U,
            .computeGradient = 0U,
            .gradientFilterMode = 0U,
            .gradientFilterPass = 0U,
            .hasGradientVariance = guides.hasGradientVariance ? 1U : 0U,
            .gradientAlpha = m_settings.gradientAlpha,
            .convergingMode = 1U, // mirror the clean chain (constant-W lerp skipped)
            .perturbedMode = 1U,
            .sampleCount = 0U,
            .prevSampleCount = 0U,
        };
        vkCmdPushDescriptorSet(cmd,
                               VK_PIPELINE_BIND_POINT_COMPUTE,
                               m_pipelineLayout,
                               0U,
                               static_cast<std::uint32_t>(writes.size()),
                               writes.data());
        vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0U, sizeof(push), &push);
        vkCmdDispatch(cmd, groupsX, groupsY, 1U);

        const std::array chainBarrier{
            core::imageBarrier(dstImage,
                               VK_IMAGE_LAYOUT_GENERAL,
                               VK_IMAGE_LAYOUT_GENERAL,
                               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                               VK_ACCESS_2_SHADER_WRITE_BIT,
                               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                               VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT),
        };
        core::pipelineBarrier(cmd, chainBarrier);

        srcView = dstView;
        srcImage = dstImage;
    }

    // Normalize the final D' into m_perturbedOutput regardless of parity.
    if (srcImage != m_perturbedOutput.handle()) {
        copyImageRoundTrip(cmd,
                           srcImage,
                           m_perturbedOutput.handle(),
                           m_extent,
                           VK_ACCESS_2_SHADER_WRITE_BIT,
                           VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    }
}

} // namespace harmonia::pipeline
