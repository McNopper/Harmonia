#include "harmonia/app/CliParser.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <slang-math/slang-math.hpp>
#include <string>
#include <string_view>

#include "harmonia/app/AppConfig.hpp"
#include "harmonia/core/Logger.hpp"

namespace harmonia::app {

std::filesystem::path CliParser::resolveScenePath(const AppConfig& config, const std::filesystem::path& sceneFile) {
    std::filesystem::path resolved =
        sceneFile.empty() ? std::filesystem::path("cornell_classic.scene.toml") : sceneFile;
    std::error_code ec;
    if (std::filesystem::exists(resolved, ec)) {
        return resolved;
    }
    const auto withExtension = [](std::filesystem::path candidate) {
        if (!candidate.string().ends_with(".scene.toml")) {
            candidate += ".scene.toml";
        }
        return candidate;
    };
    if (resolved.has_parent_path()) {
        const std::filesystem::path inside = withExtension(config.assetsDir / resolved);
        if (std::filesystem::exists(inside, ec)) {
            return inside;
        }
    }
    return withExtension(config.assetsDir / resolved.filename());
}

namespace {

// Parse a base-10 integer from a CLI argument, clamped to >= 1. Reports parse
// failures (unlike atoi, which silently returns 0) by falling back to 1.
[[nodiscard]] std::int32_t parseClampedInt(std::string_view s) noexcept {
    std::int32_t value = 0;
    const auto res = std::from_chars(s.data(), s.data() + s.size(), value);
    if (res.ec != std::errc{} || res.ptr != s.data() + s.size() || value < 1)
        return 1;
    return value;
}

[[nodiscard]] bool parseFloat(std::string_view text, float& value) noexcept {
    if (text.empty()) {
        return false;
    }
    char* end = nullptr;
    const std::string owned(text);
    const float parsed = std::strtof(owned.c_str(), &end);
    if (end != owned.c_str() + owned.size()) {
        return false;
    }
    value = parsed;
    return true;
}

} // namespace

bool CliParser::applyCommonArg(AppConfig& config, int& i, int argc, char* const argv[]) {
    const std::string_view arg = argv[i];
    const auto next = [&](const char* what) -> const char* {
        if (i + 1 >= argc) {
            core::Logger::error("Missing value for {}", what);
            return nullptr;
        }
        return argv[++i];
    };

    if (arg == "--scene" || arg == "-s") {
        if (const char* v = next("--scene")) {
            config.sceneFile = v;
        }
        return true;
    }
    if (arg == "--output" || arg == "-o") {
        if (const char* v = next("--output")) {
            config.outputFile = v;
        }
        return true;
    }
    if (arg == "--width") {
        if (const char* v = next("--width")) {
            config.width = static_cast<std::uint32_t>(parseClampedInt(v));
        }
        return true;
    }
    if (arg == "--height") {
        if (const char* v = next("--height")) {
            config.height = static_cast<std::uint32_t>(parseClampedInt(v));
        }
        return true;
    }
    if (arg == "--validation") {
        config.validation = true;
        return true;
    }
    if (arg == "--no-validation") {
        config.validation = false;
        return true;
    }
    if (arg == "--no-postfx") {
        config.noPostfx = true;
        return true;
    }
    if (arg == "--glare") {
        // Lens glare of the presentation: the share of the light the lens
        // scatters from the bright parts of the frame that the tone mapper
        // adds before its curve (0 = none). --no-postfx turns it off.
        if (const char* v = next("--glare")) {
            float glare = 0.0F;
            if (!parseFloat(v, glare) || !std::isfinite(glare) || glare < 0.0F) {
                core::Logger::error("Invalid value for --glare (must be 0 or more): {}", v);
            } else {
                config.glare = glare;
            }
        }
        return true;
    }
    if (arg == "--glare-threshold") {
        // The scene-linear luminance above which the frame's light is
        // scattered: 1.0 is the frame's reference white. A scene whose exposed
        // sky lies above its threshold gets a veil from the sky, so raise it
        // there to its light sources.
        if (const char* v = next("--glare-threshold")) {
            float threshold = 0.0F;
            if (!parseFloat(v, threshold) || !std::isfinite(threshold) || threshold <= 0.0F) {
                core::Logger::error("Invalid value for --glare-threshold (must be above 0): {}", v);
            } else {
                config.glareThreshold = threshold;
            }
        }
        return true;
    }

    if (parseDenoiserArgs(config, i, argc, argv)) {
        return true;
    }
    if (parseRenderQualityArgs(config, i, argc, argv)) {
        return true;
    }

    if (!arg.starts_with("-")) {
        config.sceneFile = std::filesystem::path(arg);
        return true;
    }
    return false;
}

bool CliParser::parseUint32(std::string_view text, std::uint32_t& value) noexcept {
    std::uint64_t parsed = 0;
    const auto begin = text.data();
    const auto end = begin + text.size();
    const auto [ptr, ec] = std::from_chars(begin, end, parsed);
    if (ec != std::errc{} || ptr != end || parsed > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    value = static_cast<std::uint32_t>(parsed);
    return true;
}

bool CliParser::parseDenoiserArgs(AppConfig& config, int& i, int argc, char* const argv[]) {
    const std::string_view arg = argv[i];
    const auto next = [&](const char* what) -> const char* {
        if (i + 1 >= argc) {
            core::Logger::error("Missing value for {}", what);
            return nullptr;
        }
        return argv[++i];
    };

    if (arg == "--denoiser-strength") {
        if (const char* v = next("--denoiser-strength")) {
            float strength = 0.0F;
            if (!parseFloat(v, strength)) {
                core::Logger::error("Invalid value for --denoiser-strength: {}", v);
            } else {
                config.denoiser.strength = sm::saturate(strength);
            }
        }
        return true;
    }
    if (arg == "--denoiser-iterations") {
        if (const char* v = next("--denoiser-iterations")) {
            std::uint32_t iterations = 0U;
            if (!CliParser::parseUint32(v, iterations)) {
                core::Logger::error("Invalid value for --denoiser-iterations: {}", v);
            } else {
                config.denoiser.iterations = std::clamp(iterations, 1U, 8U);
            }
        }
        return true;
    }
    if (arg == "--denoiser-history-blend") {
        if (const char* v = next("--denoiser-history-blend")) {
            float historyBlend = 0.0F;
            if (!parseFloat(v, historyBlend)) {
                core::Logger::error("Invalid value for --denoiser-history-blend: {}", v);
            } else {
                config.denoiser.historyBlend = sm::saturate(historyBlend);
            }
        }
        return true;
    }
    if (arg == "--denoiser-no-history") {
        config.denoiser.useHistory = false;
        return true;
    }
    if (arg == "--denoiser-history") {
        config.denoiser.useHistory = true;
        return true;
    }
    if (arg == "--denoiser-no-gradient") {
        config.denoiser.useGradient = false;
        return true;
    }
    if (arg == "--denoiser-gradient") {
        config.denoiser.useGradient = true;
        return true;
    }
    if (arg == "--denoiser-gradient-alpha") {
        if (const char* v = next("--denoiser-gradient-alpha")) {
            float gradientAlpha = 0.0F;
            if (!parseFloat(v, gradientAlpha)) {
                core::Logger::error("Invalid value for --denoiser-gradient-alpha: {}", v);
            } else {
                config.denoiser.gradientAlpha = sm::saturate(gradientAlpha);
            }
        }
        return true;
    }
    return false;
}

bool CliParser::parseRenderQualityArgs(AppConfig& config, int& i, int argc, char* const argv[]) {
    const std::string_view arg = argv[i];
    const auto next = [&](const char* what) -> const char* {
        if (i + 1 >= argc) {
            core::Logger::error("Missing value for {}", what);
            return nullptr;
        }
        return argv[++i];
    };

    if (arg == "--offscreen-frames") {
        if (const char* v = next("--offscreen-frames")) {
            std::uint32_t frames = 0U;
            if (!CliParser::parseUint32(v, frames)) {
                core::Logger::error("Invalid value for --offscreen-frames: {}", v);
            } else {
                config.offscreenFrames = std::max(frames, 1U);
            }
        }
        return true;
    }
    if (arg == "--sequence-frames") {
        // ANI7: 0 = disabled (classic single capture); N >= 1 renders N animation
        // frames to disk (per-frame EXR+PNG, tick pinned to the output frame).
        if (const char* v = next("--sequence-frames")) {
            std::uint32_t frames = 0U;
            if (!CliParser::parseUint32(v, frames)) {
                core::Logger::error("Invalid value for --sequence-frames: {}", v);
            } else {
                config.sequenceFrames = frames;
            }
        }
        return true;
    }
    if (arg == "--frame") {
        // ANI8: seek the animation start frame N (movie frame generation:
        // one run per frame with `--sequence-frames 1 --frame N`; the
        // interactive window starts at N and plays in real time).
        if (const char* v = next("--frame")) {
            std::uint32_t frame = 0U;
            if (!CliParser::parseUint32(v, frame)) {
                core::Logger::error("Invalid value for --frame: {}", v);
            } else {
                config.animationStartFrame = frame;
            }
        }
        return true;
    }
    if (arg == "--capture-every-ms") {
        // ANI7 timing extension: sample the animation every N milliseconds of
        // animation time (requires --sequence-frames; sub-frame sampling at
        // e.g. 16.67 ms, or multi-frame jumps at e.g. 1000 ms).
        if (const char* v = next("--capture-every-ms")) {
            try {
                const double ms = std::stod(v);
                if (ms <= 0.0) {
                    core::Logger::error("Invalid value for --capture-every-ms (must be > 0): {}", v);
                } else {
                    config.captureIntervalMs = ms;
                }
            } catch (...) {
                core::Logger::error("Invalid value for --capture-every-ms: {}", v);
            }
        }
        return true;
    }
    if (arg == "--start-seconds") {
        // ANI7 timing extension: explicit capture start in SECONDS (overrides
        // the --frame / fps derivation of the interval start).
        if (const char* v = next("--start-seconds")) {
            try {
                const double seconds = std::stod(v);
                if (seconds < 0.0) {
                    core::Logger::error("Invalid value for --start-seconds (must be >= 0): {}", v);
                } else {
                    config.captureStartSeconds = seconds;
                }
            } catch (...) {
                core::Logger::error("Invalid value for --start-seconds: {}", v);
            }
        }
        return true;
    }
    if (arg == "--shutter") {
        // Motion blur for a sequence capture: the fraction of one frame interval
        // the shutter is open (0.5 = a 180 degree shutter). Each accumulation
        // sample of an output frame then renders at its own time inside it.
        if (const char* v = next("--shutter")) {
            try {
                const float shutter = std::stof(v);
                if (shutter < 0.0f || shutter > 1.0f) {
                    core::Logger::error("Invalid value for --shutter (must be within 0..1): {}", v);
                } else {
                    config.shutter = shutter;
                }
            } catch (...) {
                core::Logger::error("Invalid value for --shutter: {}", v);
            }
        }
        return true;
    }
    if (arg == "--time-samples") {
        if (const char* v = next("--time-samples")) {
            std::uint32_t samples = 0U;
            if (!CliParser::parseUint32(v, samples) || samples == 0U) {
                core::Logger::error("Invalid value for --time-samples (must be >= 1): {}", v);
            } else {
                config.timeSamples = samples;
            }
        }
        return true;
    }
    if (arg == "--frames-per-flip") {
        if (const char* v = next("--frames-per-flip")) {
            std::uint32_t frames = 0U;
            if (!CliParser::parseUint32(v, frames)) {
                core::Logger::error("Invalid value for --frames-per-flip: {}", v);
            } else {
                config.framesPerFlip = std::max(frames, 1U);
            }
        }
        return true;
    }
    if (arg == "--indirect-ambient") {
        if (const char* v = next("--indirect-ambient")) {
            config.indirectAmbient = std::stof(v);
        }
        return true;
    }
    if (arg == "--ibl-diffuse-resolution") {
        if (const char* v = next("--ibl-diffuse-resolution")) {
            config.iblDiffuseResolution = static_cast<std::uint32_t>(parseClampedInt(v));
        }
        return true;
    }
    if (arg == "--display-overlay") {
        config.displayOverlay = true;
        return true;
    }
    if (arg == "--deterministic-replay") {
        config.deterministicReplay = true;
        return true;
    }
    if (arg == "--rng-debug") {
        config.rngDebug = true;
        return true;
    }
    if (arg == "--rng-seed") {
        if (const char* v = next("--rng-seed")) {
            std::uint32_t seed = 0U;
            if (!CliParser::parseUint32(v, seed)) {
                core::Logger::error("Invalid value for --rng-seed: {}", v);
            } else {
                config.rngSeed = seed;
            }
        }
        return true;
    }
    return false;
}

} // namespace harmonia::app
