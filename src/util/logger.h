#pragma once

#include <cstdio>
#include <mutex>
#include <string>

namespace hftarb {

enum class LogLevel : int { Debug = 0, Info = 1, Warn = 2, Error = 3, None = 4 };

// Minimal thread-safe logger. Console by default; optionally also a file.
class Logger {
public:
    static Logger& instance();

    void setLevel(LogLevel level) { level_ = level; }
    void setFile(const std::string& path);

    void log(LogLevel level, const std::string& tag, const std::string& msg);

    void debug(const std::string& tag, const std::string& msg) { log(LogLevel::Debug, tag, msg); }
    void info(const std::string& tag, const std::string& msg) { log(LogLevel::Info, tag, msg); }
    void warn(const std::string& tag, const std::string& msg) { log(LogLevel::Warn, tag, msg); }
    void error(const std::string& tag, const std::string& msg) { log(LogLevel::Error, tag, msg); }

private:
    Logger() = default;
    LogLevel level_ = LogLevel::Info;
    std::mutex mutex_;
    std::FILE* file_ = nullptr;
};

}  // namespace hftarb
