// SceneOutputCopyPassDenoise.cpp - the A-SVGF temporal-history machinery of
// SceneOutputCopyPass: gradient/variance dispatch, gradient triangle filter,
// barrier choreography and the image round-trip. Methods of
// harmonia::pipeline::SceneOutputCopyPass split out of SceneOutputCopyPass.cpp
// (TU split, no behavior change).
#include "harmonia/pipeline/SceneOutputCopyPass.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <utility>
#include "harmonia/core/Barrier.hpp"
#include "harmonia/core/Logger.hpp"
#include "harmonia/core/Sampler.hpp"
#include "harmonia/core/ShaderModule.hpp"
#include "harmonia/pipeline/PassContext.hpp"
#include "harmonia/pipeline/SceneOutputCopyPassDetail.hpp"

namespace harmonia::pipeline {

void SceneOutputCopyPass::barrierForComputeRead(VkCommandBuffer cmd,
                                                VkImage hdrImage,
                                                VkImage denoisedImage,
                                                bool useGradient) noexcept {
    const std::array preComputeBarriers{
        core::imageBarrier(hdrImage,
                     VK_IMAGE_LAYOUT_GENERAL,
                     VK_IMAGE_LAYOUT_GENERAL,
                     VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                     VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                         VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_SHADER_READ_BIT),
        core::imageBarrier(denoisedImage,
                     m_firstUse ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL,
                     VK_IMAGE_LAYOUT_GENERAL,
                     m_firstUse ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     m_firstUse ? 0U : VK_ACCESS_2_SHADER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT),
        core::imageBarrier(m_workImage.handle(),
                     m_firstUse ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL,
                     VK_IMAGE_LAYOUT_GENERAL,
                     m_firstUse ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     m_firstUse ? 0U : VK_ACCESS_2_SHADER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT),
        core::imageBarrier(m_historyImage.handle(),
                     m_historyFirstUse ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL,
                     VK_IMAGE_LAYOUT_GENERAL,
                     m_historyFirstUse ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     m_historyFirstUse ? 0U : VK_ACCESS_2_SHADER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT),
        // Dummy 1×1 R32G32F image always bound at binding 5; transition UNDEFINED→GENERAL
        // on first use so the descriptor's imageLayout claim is valid even when no real
        // motion-vector pass has run (hasMotionVectors == 0 prevents any actual read).
        core::imageBarrier(m_dummyMotionVectors.handle(),
                     m_firstUse ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL,
                     VK_IMAGE_LAYOUT_GENERAL,
                     m_firstUse ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     m_firstUse ? 0U : (VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT),
                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT),
    };
    core::pipelineBarrier(cmd, preComputeBarriers);

    // Gradient/variance buffers (bindings 6/7) → GENERAL for the compute passes.
    // Real full-res images share the history lifetime (UNDEFINED on first use);
    // when the gradient path is off the 1×1 dummy is transitioned instead so its
    // descriptor binding stays valid without a real read.
    if (useGradient) {
        const std::array gradientBarriers{
            core::imageBarrier(m_gradientImage.handle(),
                         m_historyFirstUse ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL,
                         VK_IMAGE_LAYOUT_GENERAL,
                         m_historyFirstUse ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                         m_historyFirstUse ? 0U : (VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT),
                         VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                         VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT),
            core::imageBarrier(m_prevGradientImage.handle(),
                         m_historyFirstUse ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL,
                         VK_IMAGE_LAYOUT_GENERAL,
                         m_historyFirstUse ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                         m_historyFirstUse ? 0U : (VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT),
                         VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                         VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT),
        };
        core::pipelineBarrier(cmd, gradientBarriers);
    } else {
        const std::array dummyGradientBarrier{
            core::imageBarrier(m_dummyGradient.handle(),
                         m_firstUse ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL,
                         VK_IMAGE_LAYOUT_GENERAL,
                         m_firstUse ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                         m_firstUse ? 0U : (VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT),
                         VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                         VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT),
        };
        core::pipelineBarrier(cmd, dummyGradientBarrier);
    }
}

VkImage SceneOutputCopyPass::recordSpatialPasses(VkCommandBuffer cmd,
                                                 const core::Image& hdrBuffer,
                                                 const core::Image& denoised,
                                                 const DispatchGrid& grid,
                                                 const DenoiseGuides& guides) noexcept {
    // Unpack the parameter objects into the historical local names (read-only below).
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
    const bool hasGradientVariance = guides.hasGradientVariance;
    VkImage currentSource = hdrBuffer.handle();
    VkImageView currentSourceView = hdrBuffer.view();
    VkImage finalSource = currentSource;

    for (std::uint32_t passIndex = 0U; passIndex < iterations; ++passIndex) {
        const bool writeToDenoised = ((passIndex & 1U) == 0U);
        const VkImageView dstView = writeToDenoised ? denoised.view() : m_workImage.view();
        finalSource = writeToDenoised ? denoised.handle() : m_workImage.handle();

        const VkDescriptorImageInfo srcInfo{
            .sampler = VK_NULL_HANDLE,
            .imageView = currentSourceView,
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        };
        const VkDescriptorImageInfo dstInfo{
            .sampler = VK_NULL_HANDLE,
            .imageView = dstView,
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
        const VkDescriptorImageInfo motionVecInfo{
            .sampler = VK_NULL_HANDLE,
            .imageView = motionVecView,
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        };
        const VkDescriptorImageInfo gradientInfo{
            .sampler = VK_NULL_HANDLE,
            .imageView = gradientView,
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        };
        const VkDescriptorImageInfo prevGradientInfo{
            .sampler = VK_NULL_HANDLE,
            .imageView = prevGradientView,
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
                .pImageInfo = &motionVecInfo,
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
        };

        const DenoiserPushConstants push{
            .strength = m_settings.strength,
            .historyBlend = m_settings.historyBlend,
            .hasNormalGuide = hasNormalGuide ? 1U : 0U,
            .hasDepthGuide = hasDepthGuide ? 1U : 0U,
            .applyHistory = 0U,
            .historyFirstUse = m_historyFirstUse ? 1U : 0U,
            .iterations = iterations,
            .passIndex = passIndex,
            .hasMotionVectors = 0U, // spatial passes never read motion vectors
            .computeGradient = 0U,  // gradient is computed in the temporal pass only
            .gradientFilterMode = 0U,
            .gradientFilterPass = 0U,
            .hasGradientVariance = hasGradientVariance ? 1U : 0U, // variance-guided luma edge stopping
            .gradientAlpha = m_settings.gradientAlpha,
        };
        vkCmdPushDescriptorSet(cmd,
                               VK_PIPELINE_BIND_POINT_COMPUTE,
                               m_pipelineLayout,
                               0U,
                               static_cast<std::uint32_t>(writes.size()),
                               writes.data());
        vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0U, sizeof(push), &push);
        vkCmdDispatch(cmd, groupsX, groupsY, 1U);

        currentSource = writeToDenoised ? denoised.handle() : m_workImage.handle();
        currentSourceView = writeToDenoised ? denoised.view() : m_workImage.view();

        if (passIndex + 1U < iterations) {
            const std::array passBarrier{
                core::imageBarrier(currentSource,
                             VK_IMAGE_LAYOUT_GENERAL,
                             VK_IMAGE_LAYOUT_GENERAL,
                             VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                             VK_ACCESS_2_SHADER_WRITE_BIT,
                             VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                             VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT),
            };
            core::pipelineBarrier(cmd, passBarrier);
        }
    }

    return finalSource;
}

void SceneOutputCopyPass::copyImageRoundTrip(VkCommandBuffer cmd,
                                             VkImage srcImage,
                                             VkImage dstImage,
                                             VkExtent2D extent,
                                             VkAccessFlags2 preSrcAccess,
                                             VkPipelineStageFlags2 restoreStage) noexcept {
    const std::array copyPrep{
        core::imageBarrier(srcImage,
                     VK_IMAGE_LAYOUT_GENERAL,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     preSrcAccess,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                     VK_ACCESS_2_TRANSFER_READ_BIT),
        core::imageBarrier(dstImage,
                     VK_IMAGE_LAYOUT_GENERAL,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     preSrcAccess,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                     VK_ACCESS_2_TRANSFER_WRITE_BIT),
    };
    core::pipelineBarrier(cmd, copyPrep);

    const VkImageCopy copyRegion{
        .srcSubresource = VkImageSubresourceLayers{VK_IMAGE_ASPECT_COLOR_BIT, 0U, 0U, 1U},
        .srcOffset = VkOffset3D{0, 0, 0},
        .dstSubresource = VkImageSubresourceLayers{VK_IMAGE_ASPECT_COLOR_BIT, 0U, 0U, 1U},
        .dstOffset = VkOffset3D{0, 0, 0},
        .extent = VkExtent3D{extent.width, extent.height, 1U},
    };
    vkCmdCopyImage(cmd,
                   srcImage,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   dstImage,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1U,
                   &copyRegion);

    const std::array copyRestore{
        core::imageBarrier(srcImage,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_IMAGE_LAYOUT_GENERAL,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                     VK_ACCESS_2_TRANSFER_READ_BIT,
                     restoreStage,
                     VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT),
        core::imageBarrier(dstImage,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_GENERAL,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                     VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     restoreStage,
                     VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT),
    };
    core::pipelineBarrier(cmd, copyRestore);
}

void SceneOutputCopyPass::recordTemporalHistoryPass(VkCommandBuffer cmd,
                                                    const core::Image& denoised,
                                                    bool useGradient,
                                                    bool hasMotionVectors,
                                                    const DispatchGrid& grid,
                                                    const DenoiseGuides& guides) noexcept {
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
        .hasGradientVariance = 0U, // temporal pass writes, does not read, the variance
        .gradientAlpha = m_settings.gradientAlpha,
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

void SceneOutputCopyPass::restoreBarriers(VkCommandBuffer cmd, VkImage hdrImage, VkImage denoisedImage) noexcept {
    const std::array restoreBarriers{
        core::imageBarrier(hdrImage,
                     VK_IMAGE_LAYOUT_GENERAL,
                     VK_IMAGE_LAYOUT_GENERAL,
                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_SHADER_READ_BIT,
                     VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                     VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT |
                         VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT),
        core::imageBarrier(denoisedImage,
                     VK_IMAGE_LAYOUT_GENERAL,
                     VK_IMAGE_LAYOUT_GENERAL,
                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_SHADER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                     VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT |
                         VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT),
        core::imageBarrier(m_workImage.handle(),
                     VK_IMAGE_LAYOUT_GENERAL,
                     VK_IMAGE_LAYOUT_GENERAL,
                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                     VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT),
        core::imageBarrier(m_historyImage.handle(),
                     VK_IMAGE_LAYOUT_GENERAL,
                     VK_IMAGE_LAYOUT_GENERAL,
                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                     VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT),
    };
    core::pipelineBarrier(cmd, restoreBarriers);
}


} // namespace harmonia::pipeline
