#ifndef HARMONIA_APP_CLIPARSER_HPP
#define HARMONIA_APP_CLIPARSER_HPP

#include <cstdint>
#include <string_view>

#include "harmonia/app/AppConfig.hpp"

namespace harmonia {

class CliParser {
  public:
    [[nodiscard]] static bool applyCommonArg(AppConfig& config, int& i, int argc, char* const argv[]);
    [[nodiscard]] static bool parseUint32(std::string_view text, std::uint32_t& value) noexcept;

  private:
    [[nodiscard]] static bool parseDenoiserArgs(AppConfig& config, int& i, int argc, char* const argv[]);
    [[nodiscard]] static bool parseRenderQualityArgs(AppConfig& config, int& i, int argc, char* const argv[]);
};

} // namespace harmonia
#endif // HARMONIA_APP_CLIPARSER_HPP
