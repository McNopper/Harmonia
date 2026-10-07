#ifndef HARMONIA_CORE_ENV_HPP
#define HARMONIA_CORE_ENV_HPP

#include <cstdlib>
#include <optional>
#include <string>

namespace harmonia {

/// Read an environment variable as an owning string.
///
/// Portable replacement for the MSVC-only `_dupenv_s` that Theia's debug/A-B
/// toggles used: Windows reads through the Annex-K `_dupenv_s` (plain
/// `getenv` is deprecated in the MSVC CRT), POSIX through `getenv`. The value
/// is copied immediately, so there is no lifetime tie to the environment
/// block. Returns `std::nullopt` when the variable is unset.
[[nodiscard]] inline std::optional<std::string> getEnvString(const char* name) {
#if defined(_WIN32)
    char* raw = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&raw, &length, name) != 0 || raw == nullptr) {
        return std::nullopt;
    }
    std::string value(raw);
    std::free(raw);
    return value;
#else
    const char* raw = std::getenv(name);
    if (raw == nullptr) {
        return std::nullopt;
    }
    return std::string(raw);
#endif
}

} // namespace harmonia

#endif // HARMONIA_CORE_ENV_HPP
