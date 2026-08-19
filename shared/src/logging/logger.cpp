#include <algorithm>
#include <cctype>
#include <memory>
#include <vector>

#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <dv/logging/logger.hpp>

namespace dv::log {
namespace {

spdlog::level::level_enum to_spdlog(Level level) {
  switch (level) {
    case Level::Trace:
      return spdlog::level::trace;
    case Level::Debug:
      return spdlog::level::debug;
    case Level::Info:
      return spdlog::level::info;
    case Level::Warn:
      return spdlog::level::warn;
    case Level::Error:
      return spdlog::level::err;
    case Level::Fatal:
      return spdlog::level::critical;
    case Level::Off:
      return spdlog::level::off;
  }
  return spdlog::level::info;
}

std::string to_lower(std::string_view text) {
  std::string result(text);
  std::ranges::transform(result, result.begin(),
                         [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return result;
}

}  // namespace

void init(const Options& options) {
  std::vector<spdlog::sink_ptr> sinks;

  if (options.log_to_console) {
    auto console = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
    // Example from section 23 of SPEC.md: [INFO] Connected to signaling server
    console->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%t] %v");
    sinks.push_back(std::move(console));
  }

  if (!options.file_path.empty()) {
    auto file = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
        options.file_path, options.max_file_size_bytes, options.max_files);
    file->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%t] [%s:%#] %v");
    sinks.push_back(std::move(file));
  }

  auto logger = std::make_shared<spdlog::logger>("dv", sinks.begin(), sinks.end());
  logger->set_level(to_spdlog(options.level));
  logger->flush_on(spdlog::level::warn);

  spdlog::set_default_logger(std::move(logger));
}

void shutdown() {
  // spdlog::shutdown() drops every logger, the default one included, and the
  // macros then call through a null pointer. That is not hypothetical: media
  // callbacks and worker threads keep logging while the process tears down, and
  // one line arriving late would take it down with a segfault instead of
  // letting it exit.
  //
  // A logger that discards everything takes its place, so a late log line is
  // simply ignored.
  auto silent = std::make_shared<spdlog::logger>("dv-silent");
  silent->set_level(spdlog::level::off);

  spdlog::shutdown();
  spdlog::set_default_logger(std::move(silent));
}

void set_level(Level level) {
  spdlog::set_level(to_spdlog(level));
}

Level level_from_string(std::string_view text, Level fallback) {
  const std::string normalized = to_lower(text);
  if (normalized == "trace") {
    return Level::Trace;
  }
  if (normalized == "debug") {
    return Level::Debug;
  }
  if (normalized == "info") {
    return Level::Info;
  }
  if (normalized == "warn" || normalized == "warning") {
    return Level::Warn;
  }
  if (normalized == "error") {
    return Level::Error;
  }
  if (normalized == "fatal" || normalized == "critical") {
    return Level::Fatal;
  }
  if (normalized == "off" || normalized == "none") {
    return Level::Off;
  }
  return fallback;
}

std::string_view to_string(Level level) {
  switch (level) {
    case Level::Trace:
      return "trace";
    case Level::Debug:
      return "debug";
    case Level::Info:
      return "info";
    case Level::Warn:
      return "warn";
    case Level::Error:
      return "error";
    case Level::Fatal:
      return "fatal";
    case Level::Off:
      return "off";
  }
  return "info";
}

}  // namespace dv::log
