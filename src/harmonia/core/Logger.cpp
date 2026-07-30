#include "harmonia/core/Logger.hpp"

#include <cstdio>
#include <mutex>
#include <print>

namespace harmonia {

namespace {
std::mutex& logMutex() {
    static std::mutex mutex;
    return mutex;
}

std::string& logTag() {
    // Default tag for the shared Harmonia core; applications override via setTag().
    static std::string tag = "HARMONIA";
    return tag;
}
} // namespace

void Logger::setTag(std::string_view tag) {
    std::scoped_lock lock(logMutex());
    logTag() = std::string(tag);
}

void Logger::log(Level level, std::string message) {
    std::scoped_lock lock(logMutex());

    const char* levelString;
    switch (level) {
    case Level::Info:
        levelString = "INFO";
        break;
    case Level::Warning:
        levelString = "WARNING";
        break;
    case Level::Error:
        levelString = "ERROR";
        break;
    default:
        levelString = "UNKNOWN";
        break;
    }

    const std::string formatted = std::format("[{}][{}] {}", logTag(), levelString, message);
    std::FILE* const stream = level == Level::Error ? stderr : stdout;
    std::print(stream, "{}\n", formatted);
    std::fflush(stream);
}

} // namespace harmonia
