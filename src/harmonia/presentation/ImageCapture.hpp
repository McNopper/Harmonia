#ifndef HARMONIA_PRESENTATION_IMAGECAPTURE_HPP
#define HARMONIA_PRESENTATION_IMAGECAPTURE_HPP

#include <filesystem>

#include "harmonia/core/CommandPool.hpp"
#include "harmonia/core/Image.hpp"
#include "harmonia/utils/ColorSpace.hpp"

// Offscreen capture helpers shared by both renderers.  Hyperion (path tracer) and
// Theia (rasterizer) each render into an RGBA32F HDR image kept in
// VK_IMAGE_LAYOUT_GENERAL and use these to read it back and save it to disk.
//
// The tone-mapping (ACES SDR) and the buffer read-back are identical across the two
// renderers, so the code lives here rather than being duplicated per renderer.
namespace harmonia::ImageCapture {

// Reads back the working-space linear RGBA32F HDR image, applies the ACES SDR
// tone map (working space -> Rec.709 linear -> sRGB) and writes an 8-bit RGB PNG.
// workingSpace declares the scene-referred space of hdrImage's contents.
// The image must be in VK_IMAGE_LAYOUT_GENERAL on entry; it is restored to
// VK_IMAGE_LAYOUT_GENERAL on return.  Returns false on failure.
bool savePng(const DeviceContext& ctx,
             const CommandPool& pool,
             const Image& hdrImage,
             const std::filesystem::path& path,
             ColorSpace::WorkingColorSpace workingSpace = ColorSpace::WorkingColorSpace::LinRec2020);

// Writes the linear HDR RGBA32F image to a 32-bit float RGB EXR, tagging the
// header with the working space's chromaticities so downstream tools can
// interpret the primaries.  No-op (with a warning) when OpenEXR support is not
// compiled in (HARMONIA_HAS_OPENEXR).
bool saveExr(const DeviceContext& ctx,
             const CommandPool& pool,
             const Image& hdrImage,
             const std::filesystem::path& path,
             ColorSpace::WorkingColorSpace workingSpace = ColorSpace::WorkingColorSpace::LinRec2020);

// Writes an already display-referred, tone-mapped 8-bit image (the output of the GPU
// ToneMapper display stage, e.g. an RGBA8/BGRA8 sRGB capture target) straight to an 8-bit
// RGB PNG with no further tone mapping. This is how offscreen capture matches the interactive
// window exactly: both go through the same GPU ToneMapper. `swapRB` handles BGRA swapchains.
// The image must be in VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL-capable GENERAL on entry.
bool saveSdrPng(const DeviceContext& ctx,
                const CommandPool& pool,
                const Image& sdrImage,
                const std::filesystem::path& path,
                bool swapRB);

} // namespace ImageCapture
#endif // HARMONIA_PRESENTATION_IMAGECAPTURE_HPP
