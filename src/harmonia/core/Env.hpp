#ifndef HARMONIA_CORE_ENV_HPP
#define HARMONIA_CORE_ENV_HPP

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace harmonia::core {

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

/// Directory of the running executable (deterministic across launch CWDs).
/// Falls back to the current directory when the platform path cannot be
/// resolved (overlong Windows paths, exotic non-procfs Unix).
[[nodiscard]] inline std::filesystem::path executableDirectory() {
    std::error_code ec;
#if defined(_WIN32)
    wchar_t wide[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, wide, MAX_PATH);
    if (length != 0U && length < MAX_PATH) {
        const std::filesystem::path exe(std::wstring_view{wide, length});
        if (!exe.parent_path().empty()) {
            return exe.parent_path();
        }
    }
#else
    std::string buffer(4096, '\0');
    const ssize_t length = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (length > 0) {
        buffer.resize(static_cast<size_t>(length));
        const std::filesystem::path exe(buffer);
        if (!exe.parent_path().empty()) {
            return exe.parent_path();
        }
    }
#endif
    return std::filesystem::current_path(ec);
}

} // namespace harmonia::core

#endif // HARMONIA_CORE_ENV_HPP
