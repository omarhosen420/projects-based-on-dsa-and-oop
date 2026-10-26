// MMF Engine — tiny leveled logger (header-only, thread-safe).
#pragma once

#include <atomic>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>

namespace mmf {

enum class LogLevel : std::uint8_t { Trace = 0, Info = 1, Warn = 2, Error = 3, Off = 4 };

class Logger {
 public:
  static Logger& instance() {
    static Logger logger;
    return logger;
  }

  void setLevel(LogLevel level) noexcept { level_.store(level, std::memory_order_relaxed); }
  [[nodiscard]] LogLevel level() const noexcept { return level_.load(std::memory_order_relaxed); }

  void write(LogLevel level, std::string_view scope, const std::string& message) {
    if (level < level_.load(std::memory_order_relaxed)) return;
    static const char* const names[] = {"trace", "info", "warn", "error", "off"};
    std::lock_guard<std::mutex> guard(mutex_);
    std::cerr << "[mmf][" << names[static_cast<int>(level)] << "][" << scope << "] " << message
              << '\n';
  }

 private:
  std::atomic<LogLevel> level_{LogLevel::Warn};
  std::mutex mutex_;
};

inline void logMessage(LogLevel level, std::string_view scope, const std::string& message) {
  Logger::instance().write(level, scope, message);
}

}  // namespace mmf
