#include "harmonia/scene/IblProbe.hpp"

#include <OpenImageIO/imageio.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

#include "harmonia/core/Buffer.hpp"
#include "harmonia/core/Logger.hpp"
#include "harmonia/core/Sampler.hpp"

namespace {
constexpr float kRec2020RedPrimariesThreshold = 0.68f;
} // namespace

std::expected<IblProbe, VkResult> IblProbe::loadFromEXR(const DeviceContext& ctx,
                                                        const CommandPool& pool,
                                                        const std::filesystem::path& path,
                                                        harmonia::ColorSpace::WorkingColorSpace workingSpace) {
    auto exr = readEXR(path);
    if (!exr) {
        return std::unexpected(exr.error());
    }

    std::vector<float> rgba32f = convertPrimaries(exr->raw, exr->width, exr->height, exr->srcRec2020, workingSpace);

    IblProbe probe;
    if (const VkResult result = uploadEnvPanorama(probe, ctx, pool, rgba32f, exr->width, exr->height);
        result != VK_SUCCESS) {
        return std::unexpected(result);
    }

    buildImportanceCdf(probe, ctx, rgba32f, exr->width, exr->height);

    Logger::info("IblProbe: loaded '{}' ({}×{})", path.filename().string(), exr->width, exr->height);
    return probe;
}

std::expected<IblProbe::ExrData, VkResult> IblProbe::readEXR(const std::filesystem::path& path) {
    // Disable OIIO automatic color management — Harmonia handles all color
    // conversions explicitly. Without this, OIIO may apply an OCIO transform
    // if a color config is active (e.g. chromaticity adaptation on EXR),
    // double-converting pixels that we then convert ourselves.
    OIIO::ImageSpec openConfig;
    openConfig.attribute("raw_color", 1);

    auto inp = OIIO::ImageInput::open(path.string(), &openConfig);
    if (!inp) {
        Logger::error("IblProbe: failed to open '{}': {}", path.string(), OIIO::geterror());
        return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
    }

    const OIIO::ImageSpec& spec = inp->spec();
    const std::size_t width = static_cast<std::size_t>(spec.width);
    const std::size_t height = static_cast<std::size_t>(spec.height);

    // ── Diagnostic: log channel layout so misloads are immediately visible ────
    {
        std::string chanDesc;
        for (int c = 0; c < spec.nchannels; ++c) {
            const OIIO::TypeDesc ct = spec.channelformat(c);
            chanDesc += spec.channelnames[static_cast<std::size_t>(c)];
            chanDesc += (ct == OIIO::TypeDesc::HALF) ? "(half)" : (ct == OIIO::TypeDesc::FLOAT) ? "(float)" : "(other)";
            if (c + 1 < spec.nchannels)
                chanDesc += ", ";
        }
        Logger::info("IblProbe: '{}' {}x{}  nchannels={} [{}]",
                     path.filename().string(),
                     width,
                     height,
                     spec.nchannels,
                     chanDesc);
    }
    // Absent attribute = Rec.709 primaries (OpenEXR spec default).
    // OIIO stores EXR chromaticities as float[8]: Rx Ry Gx Gy Bx By Wx Wy.
    bool srcRec2020 = false;
    const OIIO::ParamValue* chromaParam = spec.find_attribute("chromaticities");
    if (chromaParam && chromaParam->type() == OIIO::TypeDesc(OIIO::TypeDesc::FLOAT, 8)) {
        const auto* c = static_cast<const float*>(chromaParam->data());
        // Rec.2020 red primary x ≈ 0.708 vs Rec.709 x = 0.640 — coarse threshold.
        srcRec2020 = (c[0] > kRec2020RedPrimariesThreshold);
    }

    // ── Read all channels then reorder into RGBA ──────────────────────────────
    // OIIO preserves the EXR file's channel storage order, which for most
    // panoramas is alphabetical (A, B, G, R).  We must look up each channel by
    // name so that raw[] is always laid out as R,G,B,A regardless of file order.
    const std::size_t nchans = static_cast<std::size_t>(spec.nchannels);
    std::vector<float> allChans(width * height * nchans);
    if (!inp->read_image(0, 0, 0, spec.nchannels, OIIO::TypeDesc::FLOAT, allChans.data())) {
        Logger::error("IblProbe: read_image failed for '{}': {}", path.string(), inp->geterror());
        return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
    }
    inp->close();

    // Resolve named channel indices; fall back to positional if unnamed.
    const std::size_t iR = static_cast<std::size_t>(spec.channelindex("R") >= 0 ? spec.channelindex("R") : 0);
    const std::size_t iG = static_cast<std::size_t>(spec.channelindex("G") >= 0 ? spec.channelindex("G") : 1);
    const std::size_t iB = static_cast<std::size_t>(spec.channelindex("B") >= 0 ? spec.channelindex("B") : 2);
    const std::int32_t iA = spec.channelindex("A");

    ExrData data;
    data.width = width;
    data.height = height;
    data.srcRec2020 = srcRec2020;
    data.raw.resize(width * height * 4u);
    for (std::size_t i = 0; i < width * height; ++i) {
        const std::size_t base = i * nchans;
        data.raw[i * 4 + 0] = allChans[base + iR];
        data.raw[i * 4 + 1] = allChans[base + iG];
        data.raw[i * 4 + 2] = allChans[base + iB];
        data.raw[i * 4 + 3] = (iA >= 0) ? allChans[base + static_cast<std::size_t>(iA)] : 1.0f;
    }

    return data;
}

std::vector<float> IblProbe::convertPrimaries(const std::vector<float>& raw,
                                              std::size_t width,
                                              std::size_t height,
                                              bool srcRec2020,
                                              harmonia::ColorSpace::WorkingColorSpace workingSpace) {
    // ── Pick the primaries conversion (source → working space) ───────────────
    // Rec.709 → Rec.2020 (D65, IEC 61966 / BT.2087) and its inverse.
    struct Mat3 {
        float m00, m01, m02, m10, m11, m12, m20, m21, m22;
    };
    constexpr Mat3 k709To2020{
        0.6274040f, 0.3292820f, 0.0433140f, 0.0690970f, 0.9195400f, 0.0113630f, 0.0163916f, 0.0880132f, 0.8955950f};
    constexpr Mat3 k2020To709{1.6604911f,
                              -0.5876411f,
                              -0.0728499f,
                              -0.1245505f,
                              1.1328999f,
                              -0.0083494f,
                              -0.0181508f,
                              -0.1005789f,
                              1.1187297f};
    constexpr Mat3 kIdentity{1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    const bool dstRec2020 = (workingSpace == harmonia::ColorSpace::WorkingColorSpace::LinRec2020);
    const Mat3& m = (srcRec2020 == dstRec2020) ? kIdentity : (dstRec2020 ? k709To2020 : k2020To709);

    // Clamp non-finite values (e.g. half-float inf from overexposed sun disc) to
    // half-float max (65504) so the CDF importance sampler keeps the sun visible.
    auto safeVal = [](float v) -> float {
        constexpr float kHalfMax = 65504.0f;
        return std::isfinite(v) ? std::min(v, kHalfMax) : kHalfMax;
    };

    std::vector<float> rgba32f(width * height * 4u);
    for (std::size_t i = 0; i < width * height; ++i) {
        const float r = safeVal(raw[i * 4 + 0]);
        const float g = safeVal(raw[i * 4 + 1]);
        const float b = safeVal(raw[i * 4 + 2]);
        rgba32f[i * 4 + 0] = m.m00 * r + m.m01 * g + m.m02 * b;
        rgba32f[i * 4 + 1] = m.m10 * r + m.m11 * g + m.m12 * b;
        rgba32f[i * 4 + 2] = m.m20 * r + m.m21 * g + m.m22 * b;
        rgba32f[i * 4 + 3] = safeVal(raw[i * 4 + 3]);
    }
    return rgba32f;
}

VkResult IblProbe::uploadEnvPanorama(IblProbe& probe,
                                     const DeviceContext& ctx,
                                     const CommandPool& pool,
                                     const std::vector<float>& rgba32f,
                                     std::size_t width,
                                     std::size_t height) {
    // ── Upload to GPU ────────────────────────────────────────────────────────
    const VkExtent2D extent{static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)};
    const VkDeviceSize byteSize = static_cast<VkDeviceSize>(width * height) * 4u * sizeof(float);

    auto staging = Buffer::create(
        ctx, byteSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_HOST, "ibl.staging");
    if (!staging) {
        return staging.error();
    }
    staging->uploadData(rgba32f.data(), byteSize);

    auto image = Image::create(ctx,
                               extent,
                               VK_FORMAT_R32G32B32A32_SFLOAT,
                               VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                               VK_IMAGE_ASPECT_COLOR_BIT,
                               "ibl.env");
    if (!image) {
        return image.error();
    }

    auto cmd = pool.beginOneShot();
    if (!cmd) {
        return cmd.error();
    }

    image->transition(*cmd,
                      VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_PIPELINE_STAGE_2_NONE,
                      0,
                      VK_PIPELINE_STAGE_2_COPY_BIT,
                      VK_ACCESS_2_TRANSFER_WRITE_BIT);

    const VkBufferImageCopy region{
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .imageOffset = {0, 0, 0},
        .imageExtent = {extent.width, extent.height, 1u},
    };
    vkCmdCopyBufferToImage(*cmd, staging->handle(), image->handle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // Transition to SHADER_READ_ONLY_OPTIMAL covering all shader stages that may
    // sample the env panorama: fragment (Theia sky pass) and ray tracing (Hyperion).
    image->transition(*cmd,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_PIPELINE_STAGE_2_COPY_BIT,
                      VK_ACCESS_2_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                      VK_ACCESS_2_SHADER_READ_BIT);

    if (const VkResult result = pool.endOneShot(*cmd); result != VK_SUCCESS) {
        return result;
    }

    // ── Create sampler (REPEAT on U, CLAMP_TO_EDGE on V to avoid pole artefacts) ──
    const VkSamplerCreateInfo samplerInfo = harmonia::makeSamplerCreateInfo({
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
    });
    VkSampler sampler = VK_NULL_HANDLE;
    if (const VkResult result = vkCreateSampler(ctx.device, &samplerInfo, nullptr, &sampler); result != VK_SUCCESS) {
        return result;
    }

    probe.m_image = std::move(*image);
    probe.m_sampler = harmonia::UniqueSampler{ctx.device, sampler};
    return VK_SUCCESS;
}

void IblProbe::extractDominantSun(IblProbe& probe,
                                  float sunBestAvg,
                                  std::size_t sunBestU,
                                  std::size_t sunBestV,
                                  double sunAvgSum,
                                  std::size_t sunAvgCount) {
    static constexpr std::size_t kCdfW = 256;
    static constexpr std::size_t kCdfH = 128;
    const float kPiCpu = 3.14159265358979f;
    // Convert the brightest grid cell into a world-space direction toward the sun.
    // Inverts the lat-long convention used by the shaders (env.slang):
    //   u = atan2(z,x)/(2π) + 0.5,  v = acos(y)/π  (v=0 at top, y=+1)
    sm::float3 domSunDir{0.0f, 1.0f, 0.0f};
    float domSunStrength = 0.0f;
    {
        const float uNorm = (static_cast<float>(sunBestU) + 0.5f) / static_cast<float>(kCdfW);
        const float vNorm = (static_cast<float>(sunBestV) + 0.5f) / static_cast<float>(kCdfH);
        const float phi = (uNorm - 0.5f) * 2.0f * kPiCpu;
        const float theta = vNorm * kPiCpu;
        const float sinT = std::sin(theta);
        domSunDir = sm::normalize(sm::float3{sinT * std::cos(phi), std::cos(theta), sinT * std::sin(phi)});
        // Concentration: ratio of the brightest cell to the mean. Uniform/overcast skies
        // give a ratio near 1 (no harsh shadows); a clear sun gives a very large ratio.
        const float meanAvg =
            (sunAvgCount > 0) ? static_cast<float>(sunAvgSum / static_cast<double>(sunAvgCount)) : 0.0f;
        const float ratio = (meanAvg > 1e-8f) ? (sunBestAvg / meanAvg) : 0.0f;
        domSunStrength = std::clamp((ratio - 4.0f) / 16.0f, 0.0f, 1.0f);
        Logger::info("IblProbe: dominant light dir ({:.2f}, {:.2f}, {:.2f}), strength {:.2f} (peak/mean {:.1f})",
                     domSunDir.x,
                     domSunDir.y,
                     domSunDir.z,
                     domSunStrength,
                     ratio);
    }
    probe.m_sunDirection = domSunDir;
    probe.m_sunStrength = domSunStrength;
}

void IblProbe::buildImportanceCdf(IblProbe& probe,
                                  const DeviceContext& ctx,
                                  const std::vector<float>& rgba32f,
                                  std::size_t width,
                                  std::size_t height) {
    // ── Build 2D separable CDF for env importance sampling ───────────────────
    // Ref: PBR Book 4th ed §12.5 "Infinite Area Lights" — 2D separable CDF construction
    // Resolution: 256×128 (each cell covers ~16×16 source pixels for a 4K panorama)
    static constexpr std::size_t kCdfW = 256;
    static constexpr std::size_t kCdfH = 128;
    const float kPiCpu = 3.14159265358979f;
    const float srcToGridU = static_cast<float>(kCdfW) / static_cast<float>(width);
    const float srcToGridV = static_cast<float>(kCdfH) / static_cast<float>(height);
    // Map a CDF grid coordinate to the first source pixel it covers (CDF cell →
    // source-image pixel), truncating the float grid→pixel ratio to an index.
    const auto srcPixel = [](std::size_t grid, float srcToGrid) noexcept -> std::size_t {
        return static_cast<std::size_t>(static_cast<float>(grid) / srcToGrid);
    };

    // Luminance grid: weighted by sin(θ) to account for equirectangular → solid-angle mapping
    std::vector<float> lumGrid(kCdfW * kCdfH);
    // Track the brightest (raw, unweighted) cell to extract a dominant "sun" direction
    // for ray-traced directional shadows, plus the mean to gauge how concentrated it is.
    float sunBestAvg = -1.0f;
    std::size_t sunBestU = 0, sunBestV = 0;
    double sunAvgSum = 0.0;
    std::size_t sunAvgCount = 0;
    for (std::size_t v = 0; v < kCdfH; ++v) {
        const float sinTheta = std::sin(kPiCpu * (static_cast<float>(v) + 0.5f) / static_cast<float>(kCdfH));
        for (std::size_t u = 0; u < kCdfW; ++u) {
            const std::size_t srcX0 = srcPixel(u, srcToGridU);
            const std::size_t srcX1 = std::max(srcX0 + 1, srcPixel(u + 1, srcToGridU));
            const std::size_t srcY0 = srcPixel(v, srcToGridV);
            const std::size_t srcY1 = std::max(srcY0 + 1, srcPixel(v + 1, srcToGridV));
            const std::size_t cX1 = std::min(srcX1, width);
            const std::size_t cY1 = std::min(srcY1, height);

            float sumLum = 0.0f;
            std::size_t count = 0;
            for (std::size_t sy = srcY0; sy < cY1; ++sy) {
                for (std::size_t sx = srcX0; sx < cX1; ++sx) {
                    const std::size_t idx = (sy * width + sx) * 4u;
                    const float r = rgba32f[idx + 0];
                    const float g = rgba32f[idx + 1];
                    const float b = rgba32f[idx + 2];
                    // Rec.2020 luminance coefficients (ITU-R BT.2020)
                    const float lum = 0.2627f * r + 0.6780f * g + 0.0593f * b;
                    sumLum += (std::isfinite(lum) && lum > 0.0f) ? lum : 0.0f;
                    ++count;
                }
            }
            const float avgLum = (count > 0 ? sumLum / static_cast<float>(count) : 0.0f);
            sunAvgSum += avgLum;
            ++sunAvgCount;
            if (avgLum > sunBestAvg) {
                sunBestAvg = avgLum;
                sunBestU = u;
                sunBestV = v;
            }
            lumGrid[v * kCdfW + u] = avgLum * sinTheta;
        }
    }

    extractDominantSun(probe, sunBestAvg, sunBestU, sunBestV, sunAvgSum, sunAvgCount);

    // Per-row conditional CDFs: conditionalCdf[v*(W+1)..(v+1)*(W+1)] for each row v
    std::vector<float> conditionalCdf(kCdfH * (kCdfW + 1));
    std::vector<float> rowIntegrals(kCdfH, 0.0f);
    for (std::size_t v = 0; v < kCdfH; ++v) {
        float* rowCdf = conditionalCdf.data() + static_cast<std::ptrdiff_t>(v * (kCdfW + 1));
        rowCdf[0] = 0.0f;
        for (std::size_t u = 0; u < kCdfW; ++u) {
            rowCdf[u + 1] = rowCdf[u] + lumGrid[v * kCdfW + u];
        }
        rowIntegrals[v] = rowCdf[kCdfW];
        if (rowIntegrals[v] > 0.0f) {
            const float invRow = 1.0f / rowIntegrals[v];
            for (std::size_t u = 1; u <= kCdfW; ++u) {
                rowCdf[u] *= invRow;
            }
        } else {
            // Degenerate dark row: uniform distribution so binary search returns valid indices
            for (std::size_t u = 1; u <= kCdfW; ++u) {
                rowCdf[u] = static_cast<float>(u) / static_cast<float>(kCdfW);
            }
        }
        rowCdf[kCdfW] = 1.0f; // ensure exact 1.0
    }

    // Marginal CDF over rows: marginalCdf[H+1]
    std::vector<float> marginalCdf(kCdfH + 1);
    marginalCdf[0] = 0.0f;
    for (std::size_t v = 0; v < kCdfH; ++v) {
        marginalCdf[v + 1] = marginalCdf[v] + rowIntegrals[v];
    }
    const float totalWeight = marginalCdf[kCdfH];
    if (totalWeight > 0.0f) {
        const float invTotal = 1.0f / totalWeight;
        for (std::size_t v = 1; v <= kCdfH; ++v) {
            marginalCdf[v] *= invTotal;
        }
        marginalCdf[kCdfH] = 1.0f;

        // Upload marginal CDF buffer
        const VkDeviceSize margSize = static_cast<VkDeviceSize>(kCdfH + 1) * sizeof(float);
        auto mBuf = Buffer::create(
            ctx, margSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, "ibl.marginalCdf");
        if (mBuf) {
            mBuf->uploadData(marginalCdf.data(), margSize);
            probe.m_marginalCdf = std::move(*mBuf);
        }

        // Upload conditional CDF buffer
        const VkDeviceSize condSize = static_cast<VkDeviceSize>(kCdfH * (kCdfW + 1)) * sizeof(float);
        auto cBuf = Buffer::create(ctx,
                                   condSize,
                                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                   VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
                                   "ibl.conditionalCdf");
        if (cBuf) {
            cBuf->uploadData(conditionalCdf.data(), condSize);
            probe.m_conditionalCdf = std::move(*cBuf);
        }

        if (probe.m_marginalCdf.isValid() && probe.m_conditionalCdf.isValid()) {
            probe.m_cdfWidth = static_cast<std::uint32_t>(kCdfW);
            probe.m_cdfHeight = static_cast<std::uint32_t>(kCdfH);
            Logger::info("IblProbe: built {}×{} importance CDF (total weight {:.2f})", kCdfW, kCdfH, totalWeight);
        }
    } else {
        Logger::warn("IblProbe: env map is completely dark — importance sampling disabled");
    }
}
