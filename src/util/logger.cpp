#include "src/util/logger.h"

#include <chrono>
#include <cstring>
#include <ctime>

namespace hftarb {

Logger& Logger::instance() {
    static Logger instance;
    return instance;
}

void Logger::setFile(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_) std::fclose(file_);
    file_ = std::fopen(path.c_str(), "a");
}

void Logger::log(LogLevel level, const std::string& tag, const std::string& msg) {
    if (level < level_) return;

    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char timebuf[32];
    std::strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", &tm);

    static const char* levelStr[] = {"DEBUG", "INFO", "WARN", "ERROR"};
    const int idx = static_cast<int>(level);

    std::lock_guard<std::mutex> lock(mutex_);
    std::fprintf(stdout, "[%s] [%s] [%s] %s\n", timebuf, levelStr[idx], tag.c_str(), msg.c_str());
    std::fflush(stdout);
    if (file_) {
        std::fprintf(file_, "[%s] [%s] [%s] %s\n", timebuf, levelStr[idx], tag.c_str(), msg.c_str());
        std::fflush(file_);
    }
}

}  // namespace hftarb
