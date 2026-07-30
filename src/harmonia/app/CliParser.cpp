#include "harmonia/app/CliParser.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>

#include "harmonia/app/AppConfig.hpp"
#include "harmonia/core/Logger.hpp"

namespace harmonia {

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
            Logger::error("Missing value for {}", what);
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
            Logger::error("Missing value for {}", what);
            return nullptr;
        }
        return argv[++i];
    };

    if (arg == "--denoiser-strength") {
        if (const char* v = next("--denoiser-strength")) {
            float strength = 0.0F;
            if (!parseFloat(v, strength)) {
                Logger::error("Invalid value for --denoiser-strength: {}", v);
            } else {
                config.denoiser.strength = std::clamp(strength, 0.0F, 1.0F);
            }
        }
        return true;
    }
    if (arg == "--denoiser-iterations") {
        if (const char* v = next("--denoiser-iterations")) {
            std::uint32_t iterations = 0U;
            if (!CliParser::parseUint32(v, iterations)) {
                Logger::error("Invalid value for --denoiser-iterations: {}", v);
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
                Logger::error("Invalid value for --denoiser-history-blend: {}", v);
            } else {
                config.denoiser.historyBlend = std::clamp(historyBlend, 0.0F, 1.0F);
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
                Logger::error("Invalid value for --denoiser-gradient-alpha: {}", v);
            } else {
                config.denoiser.gradientAlpha = std::clamp(gradientAlpha, 0.0F, 1.0F);
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
            Logger::error("Missing value for {}", what);
            return nullptr;
        }
        return argv[++i];
    };

    if (arg == "--offscreen-frames") {
        if (const char* v = next("--offscreen-frames")) {
            std::uint32_t frames = 0U;
            if (!CliParser::parseUint32(v, frames)) {
                Logger::error("Invalid value for --offscreen-frames: {}", v);
            } else {
                config.offscreenFrames = std::max(frames, 1U);
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
    if (arg == "--rt-gi") {
        config.rtGi = true;
        return true;
    }
    if (arg == "--no-rt-gi") {
        config.rtGi = false;
        return true;
    }
    if (arg == "--rng-seed") {
        if (const char* v = next("--rng-seed")) {
            std::uint32_t seed = 0U;
            if (!CliParser::parseUint32(v, seed)) {
                Logger::error("Invalid value for --rng-seed: {}", v);
            } else {
                config.rngSeed = seed;
            }
        }
        return true;
    }
    return false;
}

} // namespace harmonia
