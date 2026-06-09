#pragma once

#include <filesystem>

#include "harmonia/core/CommandPool.hpp"
#include "harmonia/core/Image.hpp"

// Offscreen capture helpers shared by both renderers.  Hyperion (path tracer) and
// Theia (rasterizer) each render into an RGBA32F HDR image kept in
// VK_IMAGE_LAYOUT_GENERAL and use these to read it back and save it to disk.
//
// The tone-mapping (ACES SDR) and the buffer read-back are identical across the two
// renderers, so the code lives here rather than being duplicated per renderer.
namespace ImageCapture {

// Reads back the linear Rec.2020 RGBA32F HDR image, applies the ACES SDR tone map
// (Rec.2020 linear -> Rec.709 linear -> sRGB) and writes an 8-bit RGB PNG.
// The image must be in VK_IMAGE_LAYOUT_GENERAL on entry; it is restored to
// VK_IMAGE_LAYOUT_GENERAL on return.  Returns false on failure.
bool savePng(const DeviceContext& ctx,
             const CommandPool& pool,
             const Image& hdrImage,
             const std::filesystem::path& path);

// Writes the linear HDR RGBA32F image to a 32-bit float RGB EXR.  No-op (with a
// warning) when OpenEXR support is not compiled in (HARMONIA_HAS_OPENEXR).
bool saveExr(const DeviceContext& ctx,
             const CommandPool& pool,
             const Image& hdrImage,
             const std::filesystem::path& path);

} // namespace ImageCapture
