#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include <spdlog/spdlog.h>

/// Structured logging, covering the levels listed in section 23 of SPEC.md.
///
/// FATAL maps onto spdlog's `critical`, which is the same severity under a
/// different name. Everything else maps one to one.
namespace dv::log {

enum class Level : std::uint8_t { Trace, Debug, Info, Warn, Error, Fatal, Off };

struct Options {
  Level level = Level::Info;
  /// When empty, logs go to the console only.
  std::string file_path;
  bool log_to_console = true;
  /// Rotating file logs, so a long call cannot fill the disk.
  std::size_t max_file_size_bytes = 10UL * 1024 * 1024;
  std::size_t max_files = 3;
};

/// Installs the process wide logger. Safe to call more than once; the last
/// call wins.
void init(const Options& options);

/// Flushes and releases the logger. Called automatically at exit.
void shutdown();

void set_level(Level level);

/// Parses "trace", "debug", "info", "warn", "error", "fatal" or "off",
/// case insensitively. Unknown input yields `fallback`.
[[nodiscard]] Level level_from_string(std::string_view text, Level fallback = Level::Info);

[[nodiscard]] std::string_view to_string(Level level);

}  // namespace dv::log

// The macros carry source location and are compiled out below
// SPDLOG_ACTIVE_LEVEL, so a TRACE call costs nothing in a release build.
#define DV_LOG_TRACE(...) SPDLOG_TRACE(__VA_ARGS__)
#define DV_LOG_DEBUG(...) SPDLOG_DEBUG(__VA_ARGS__)
#define DV_LOG_INFO(...) SPDLOG_INFO(__VA_ARGS__)
#define DV_LOG_WARN(...) SPDLOG_WARN(__VA_ARGS__)
#define DV_LOG_ERROR(...) SPDLOG_ERROR(__VA_ARGS__)
#define DV_LOG_FATAL(...) SPDLOG_CRITICAL(__VA_ARGS__)
