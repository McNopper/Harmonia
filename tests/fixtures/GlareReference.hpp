#ifndef TESTS_FIXTURES_GLAREREFERENCE_HPP
#define TESTS_FIXTURES_GLAREREFERENCE_HPP

// Reference of the lens glare (harmonia::pipeline::GlarePass, shaders/glare.slang) in
// double precision. It is written from the description of the algorithm in
// GlarePass.hpp, not from the shader's code, and the tests compare the GPU with it.
//
//   1. Level 0, at half the resolution of the frame: a 13-tap downsample of the frame,
//      every texel of it first reduced to what lies above a soft knee around the
//      threshold.
//   2. Level i + 1: the same 13-tap downsample of level i.
//   3. The upsampled chain: its coarsest level is the coarsest downsampled one; below
//      it, level i = downsampled level i + weight x (upsampled level i + 1 under a tent
//      filter two texels wide).
//
// A level exists while both of its dimensions are at least 2.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace glare_reference {

using Pixel = std::array<double, 3>;
using Plane = std::vector<Pixel>; ///< row-major, width * height

struct Size {
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;

    [[nodiscard]] bool operator==(const Size&) const = default;
};

[[nodiscard]] inline Size halfOf(Size size) noexcept {
    return {std::max(1U, (size.width + 1U) / 2U), std::max(1U, (size.height + 1U) / 2U)};
}

[[nodiscard]] inline std::size_t count(Size size) noexcept {
    return static_cast<std::size_t>(size.width) * size.height;
}

/// The sizes of the downsampled levels, from level 0 on, for a frame of `frameSize`.
[[nodiscard]] inline std::vector<Size> levelSizes(Size frameSize) {
    std::vector<Size> sizes;
    Size level = halfOf(frameSize);
    while (level.width >= 2U && level.height >= 2U) {
        sizes.push_back(level);
        level = halfOf(level);
    }
    return sizes;
}

/// A texel read outside the image gives the nearest texel. A negative component is no
/// light, and a texel with a non-finite component is no light at all.
[[nodiscard]] inline Pixel texel(const Plane& image, Size size, int x, int y) {
    const auto column = static_cast<std::size_t>(std::clamp(x, 0, static_cast<int>(size.width) - 1));
    const auto row = static_cast<std::size_t>(std::clamp(y, 0, static_cast<int>(size.height) - 1));
    const Pixel& value = image[row * size.width + column];
    for (double component : value) {
        if (!std::isfinite(component)) {
            return Pixel{0.0, 0.0, 0.0};
        }
    }
    return Pixel{std::max(value[0], 0.0), std::max(value[1], 0.0), std::max(value[2], 0.0)};
}

/// What a texel contributes to the glare: the part of its luminance above the
/// threshold, through a soft knee of half the threshold's width below it.
[[nodiscard]] inline Pixel brightPass(const Pixel& color, double threshold) {
    const double knee = 0.5 * threshold;
    const double luminance = 0.2126 * color[0] + 0.7152 * color[1] + 0.0722 * color[2];
    const double soft = std::clamp(luminance - threshold + knee, 0.0, 2.0 * knee);
    const double above = std::max(soft * soft / (4.0 * knee), luminance - threshold);
    const double contribution = above / std::max(luminance, 1.0e-5);
    return Pixel{color[0] * contribution, color[1] * contribution, color[2] * contribution};
}

/// The mean of the 2x2 texels whose top-left one is (x, y), each through the bright pass when `bright`.
[[nodiscard]] inline Pixel block(const Plane& image, Size size, int x, int y, bool bright, double threshold) {
    Pixel sum{0.0, 0.0, 0.0};
    for (const int dy : {0, 1}) {
        for (const int dx : {0, 1}) {
            const Pixel value = texel(image, size, x + dx, y + dy);
            const Pixel light = bright ? brightPass(value, threshold) : value;
            for (std::size_t component = 0; component < 3; ++component) {
                sum[component] += 0.25 * light[component];
            }
        }
    }
    return sum;
}

/// The 13-tap downsample of a source into a target of half its size (rounded up).
/// For a target texel at (x, y): the 2x2 block of source texels it covers, with weight
/// 1/8; the four blocks diagonal of it, 1/32 each; the four beside it, 1/16 each; and
/// the four blocks across its corners, 1/8 each.
[[nodiscard]] inline Plane
downsample(const Plane& source, Size sourceSize, Size targetSize, bool bright, double threshold) {
    struct Tap {
        int dx;
        int dy;
        double weight;
    };
    constexpr std::array kTaps{
        Tap{0, 0, 0.125},
        Tap{-2, -2, 0.03125},
        Tap{2, -2, 0.03125},
        Tap{-2, 2, 0.03125},
        Tap{2, 2, 0.03125},
        Tap{0, -2, 0.0625},
        Tap{-2, 0, 0.0625},
        Tap{2, 0, 0.0625},
        Tap{0, 2, 0.0625},
        Tap{-1, -1, 0.125},
        Tap{1, -1, 0.125},
        Tap{-1, 1, 0.125},
        Tap{1, 1, 0.125},
    };
    Plane target(count(targetSize));
    for (std::uint32_t y = 0U; y < targetSize.height; ++y) {
        for (std::uint32_t x = 0U; x < targetSize.width; ++x) {
            Pixel sum{0.0, 0.0, 0.0};
            for (const Tap& tap : kTaps) {
                const Pixel value = block(source,
                                          sourceSize,
                                          2 * static_cast<int>(x) + tap.dx,
                                          2 * static_cast<int>(y) + tap.dy,
                                          bright,
                                          threshold);
                for (std::size_t component = 0; component < 3; ++component) {
                    sum[component] += tap.weight * value[component];
                }
            }
            target[static_cast<std::size_t>(y) * targetSize.width + x] = sum;
        }
    }
    return target;
}

/// One level of the upsampled chain: a downsampled level, plus `weight` times the
/// upsampled level below it under a tent filter two texels wide. A weight of 0 copies.
[[nodiscard]] inline Plane
upsample(const Plane& level, Size levelSize, const Plane& coarse, Size coarseSize, double weight) {
    Plane target(count(levelSize));
    for (std::uint32_t y = 0U; y < levelSize.height; ++y) {
        for (std::uint32_t x = 0U; x < levelSize.width; ++x) {
            Pixel sum = level[static_cast<std::size_t>(y) * levelSize.width + x];
            if (weight > 0.0) {
                // The position of this texel's centre over the coarse image, and the
                // four coarse texels around it. A texel's weight is (1 - |d| / 2) / 2
                // per axis, d its distance from the centre; they sum to one.
                const double positionX = (static_cast<double>(x) + 0.5) * coarseSize.width / levelSize.width - 0.5;
                const double positionY = (static_cast<double>(y) + 0.5) * coarseSize.height / levelSize.height - 0.5;
                const int baseX = static_cast<int>(std::floor(positionX));
                const int baseY = static_cast<int>(std::floor(positionY));
                const double offsetX = positionX - baseX;
                const double offsetY = positionY - baseY;
                Pixel under{};
                for (int dy = -1; dy <= 2; ++dy) {
                    const double weightY = std::max(0.0, 1.0 - 0.5 * std::abs(static_cast<double>(dy) - offsetY));
                    for (int dx = -1; dx <= 2; ++dx) {
                        const double weightX = std::max(0.0, 1.0 - 0.5 * std::abs(static_cast<double>(dx) - offsetX));
                        const Pixel value = texel(coarse, coarseSize, baseX + dx, baseY + dy);
                        for (std::size_t component = 0; component < 3; ++component) {
                            under[component] += 0.25 * weightX * weightY * value[component];
                        }
                    }
                }
                for (std::size_t component = 0; component < 3; ++component) {
                    sum[component] += weight * under[component];
                }
            }
            target[static_cast<std::size_t>(y) * levelSize.width + x] = sum;
        }
    }
    return target;
}

namespace detail {

/// The chain from level 0 on: the downsamples, then the upsampled chain.
[[nodiscard]] inline Plane chain(Plane level0, const std::vector<Size>& sizes, double weight) {
    std::vector<Plane> down;
    down.reserve(sizes.size());
    down.push_back(std::move(level0));
    for (std::size_t index = 1; index < sizes.size(); ++index) {
        down.push_back(downsample(down[index - 1], sizes[index - 1], sizes[index], false, 1.0));
    }
    Plane up = down.back();
    for (std::size_t index = sizes.size() - 1; index-- > 0;) {
        up = upsample(down[index], sizes[index], up, sizes[index + 1], weight);
    }
    return up;
}

} // namespace detail

struct Result {
    Plane glare; ///< at half the resolution of the frame
    Size size{}; ///< the size of `glare`
    std::uint32_t levelCount = 0U;
    /// The same chain over the largest component of the light each texel contributes:
    /// the scale of the GPU's float32 rounding, which is relative to what is summed.
    Plane errorScale{};
};

/// The glare of a frame.
[[nodiscard]] inline Result glareOf(const Plane& frame, Size frameSize, double threshold, double weight) {
    const std::vector<Size> sizes = levelSizes(frameSize);
    Plane light(count(frameSize));
    Plane magnitude(count(frameSize));
    for (std::size_t index = 0; index < light.size(); ++index) {
        light[index] = brightPass(
            texel(
                frame, frameSize, static_cast<int>(index % frameSize.width), static_cast<int>(index / frameSize.width)),
            threshold);
        const double largest = *std::max_element(light[index].begin(), light[index].end());
        magnitude[index] = Pixel{largest, largest, largest};
    }

    Result result;
    result.size = sizes.front();
    result.levelCount = static_cast<std::uint32_t>(sizes.size());
    result.glare = detail::chain(downsample(light, frameSize, sizes.front(), false, 1.0), sizes, weight);
    result.errorScale = detail::chain(downsample(magnitude, frameSize, sizes.front(), false, 1.0), sizes, weight);
    return result;
}

/// The glare under bilinear filtering at the centre of the frame texel (x, y): what
/// the tone mapper adds there. Outside the glare image the nearest texel is read.
[[nodiscard]] inline Pixel glareAt(const Result& glare, Size frameSize, std::uint32_t x, std::uint32_t y) {
    const double positionX = (static_cast<double>(x) + 0.5) * glare.size.width / frameSize.width - 0.5;
    const double positionY = (static_cast<double>(y) + 0.5) * glare.size.height / frameSize.height - 0.5;
    const int baseX = static_cast<int>(std::floor(positionX));
    const int baseY = static_cast<int>(std::floor(positionY));
    const double offsetX = positionX - baseX;
    const double offsetY = positionY - baseY;
    Pixel sum{0.0, 0.0, 0.0};
    for (const int dy : {0, 1}) {
        const double weightY = (dy == 0) ? (1.0 - offsetY) : offsetY;
        for (const int dx : {0, 1}) {
            const double weightX = (dx == 0) ? (1.0 - offsetX) : offsetX;
            const Pixel value = texel(glare.glare, glare.size, baseX + dx, baseY + dy);
            for (std::size_t component = 0; component < 3; ++component) {
                sum[component] += weightX * weightY * value[component];
            }
        }
    }
    return sum;
}

} // namespace glare_reference

#endif // TESTS_FIXTURES_GLAREREFERENCE_HPP
