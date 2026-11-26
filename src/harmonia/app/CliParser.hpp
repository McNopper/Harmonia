#ifndef HARMONIA_APP_CLIPARSER_HPP
#define HARMONIA_APP_CLIPARSER_HPP

#include <cstdint>
#include <filesystem>
#include <string_view>

#include "harmonia/app/AppConfig.hpp"

namespace harmonia::app {

class CliParser {
  public:
    [[nodiscard]] static bool applyCommonArg(AppConfig& config, int& i, int argc, char* const argv[]);
    [[nodiscard]] static bool parseUint32(std::string_view text, std::uint32_t& value) noexcept;

    /// Resolve the `--scene` argument to a scene file.
    ///
    /// An existing path (absolute, or relative to the working directory) is
    /// used as-is. Anything else names a scene in the assets directory: first
    /// a path into one of its subdirectories (a Blender export is one
    /// folder), then the bare name of a scene in its root, each with an
    /// optional ".scene.toml" extension. A scene found in neither is returned
    /// against the assets root, where the loader diagnoses it.
    [[nodiscard]] static std::filesystem::path resolveScenePath(const AppConfig& config,
                                                                const std::filesystem::path& sceneFile);

  private:
    [[nodiscard]] static bool parseDenoiserArgs(AppConfig& config, int& i, int argc, char* const argv[]);
    [[nodiscard]] static bool parseRenderQualityArgs(AppConfig& config, int& i, int argc, char* const argv[]);
};

} // namespace harmonia::app
#endif // HARMONIA_APP_CLIPARSER_HPP
