#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <dv/core/result.hpp>

namespace dv::config {

/// Defaults come from sections 5.2, 6, 9 and 22 of SPEC.md.
struct VideoConfig {
  int width = 1280;
  int height = 720;
  int fps = 30;
  int min_bitrate_kbps = 1500;
  int max_bitrate_kbps = 3000;
  /// Kept as a string so adding VP9 and AV1 later needs no schema change.
  std::string codec = "H264";
};

struct AudioConfig {
  int sample_rate_hz = 48000;
  int channels = 1;
  int bitrate_kbps = 48;
  int frame_duration_ms = 20;
  bool echo_cancellation = true;
  bool noise_suppression = true;
  bool automatic_gain_control = true;
  /// Empty means the system default device.
  std::string input_device;
  std::string output_device;
};

struct NetworkConfig {
  std::string signaling_url = "ws://127.0.0.1:8080";
  std::vector<std::string> stun_servers = {"stun:stun.l.google.com:19302"};
  std::string turn_url;
  std::string turn_username;
  std::string turn_password;
  int reconnect_initial_delay_ms = 500;
  int reconnect_max_delay_ms = 30000;
};

struct LoggingConfig {
  std::string level = "info";
  std::string file_path;
  bool log_to_console = true;
};

struct ServerConfig {
  std::string bind_address = "0.0.0.0";
  std::uint16_t port = 8080;
  int max_participants_per_room = 5;
  int heartbeat_interval_ms = 5000;
  int heartbeat_timeout_ms = 15000;
  /// Development only account list. See server/src/main.cpp.
  std::string users_file;
};

struct Config {
  VideoConfig video;
  AudioConfig audio;
  NetworkConfig network;
  LoggingConfig logging;
  ServerConfig server;
};

/// Precedence, from weakest to strongest: built-in defaults, configuration
/// file, environment variables, command line arguments.
///
/// The file is optional. A missing file is not an error, but a file that
/// exists and cannot be parsed is.
///
/// The array is what main() is handed, and there is no other shape for it.
/// NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
[[nodiscard]] Result<Config> load(int argc, const char* const argv[]);

/// Same precedence, minus the command line. Useful in tests.
[[nodiscard]] Result<Config> load_from_file(const std::string& path);

[[nodiscard]] Result<Config> parse_json(const std::string& json_text, Config base);

void apply_environment(Config& config);

/// Returns the first constraint violation found, or nothing when valid.
[[nodiscard]] std::optional<Error> validate(const Config& config);

/// Serializes back to JSON, so a running client can write out its effective
/// configuration.
[[nodiscard]] std::string to_json(const Config& config);

}  // namespace dv::config
