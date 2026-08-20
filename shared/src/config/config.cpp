#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string_view>

#include <nlohmann/json.hpp>

#include <dv/config/config.hpp>

namespace dv::config {
namespace {

using nlohmann::json;

Error invalid_value(const std::string& field, const std::string& reason) {
  return Error{.code = "invalid_value", .message = field + ": " + reason};
}

std::optional<std::string> env(const char* name) {
  const char* raw = std::getenv(name);
  if (raw == nullptr || *raw == '\0') {
    return std::nullopt;
  }
  return std::string(raw);
}

bool parse_bool(std::string_view text, bool fallback) {
  if (text == "1" || text == "true" || text == "TRUE" || text == "yes" || text == "on") {
    return true;
  }
  if (text == "0" || text == "false" || text == "FALSE" || text == "no" || text == "off") {
    return false;
  }
  return fallback;
}

std::optional<int> parse_int(const std::string& text) {
  try {
    std::size_t consumed = 0;
    const int value = std::stoi(text, &consumed);
    if (consumed != text.size()) {
      return std::nullopt;
    }
    return value;
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

template <typename T>
void read_field(const json& object, const char* key, T& target) {
  if (object.contains(key) && !object.at(key).is_null()) {
    target = object.at(key).get<T>();
  }
}

void apply_env_int(const char* name, int& target) {
  if (const auto raw = env(name)) {
    if (const auto parsed = parse_int(*raw)) {
      target = *parsed;
    }
  }
}

void apply_env_string(const char* name, std::string& target) {
  if (const auto raw = env(name)) {
    target = *raw;
  }
}

void apply_env_bool(const char* name, bool& target) {
  if (const auto raw = env(name)) {
    target = parse_bool(*raw, target);
  }
}

/// Splits "--key=value" into its two halves. Returns false for anything that
/// is not in that shape.
bool split_argument(std::string_view argument, std::string_view& key, std::string_view& value) {
  if (!argument.starts_with("--")) {
    return false;
  }
  argument.remove_prefix(2);
  const auto separator = argument.find('=');
  if (separator == std::string_view::npos) {
    return false;
  }
  key = argument.substr(0, separator);
  value = argument.substr(separator + 1);
  return true;
}

}  // namespace

Result<Config> parse_json(const std::string& json_text, Config base) {
  json root = json::parse(json_text, nullptr, false);
  if (root.is_discarded()) {
    return Result<Config>::failure("invalid_json", "configuration is not valid JSON");
  }
  if (!root.is_object()) {
    return Result<Config>::failure("invalid_json", "configuration root must be an object");
  }

  try {
    if (root.contains("video")) {
      const json& video = root.at("video");
      read_field(video, "width", base.video.width);
      read_field(video, "height", base.video.height);
      read_field(video, "fps", base.video.fps);
      read_field(video, "min_bitrate_kbps", base.video.min_bitrate_kbps);
      read_field(video, "max_bitrate_kbps", base.video.max_bitrate_kbps);
      read_field(video, "floor_bitrate_kbps", base.video.floor_bitrate_kbps);
      read_field(video, "codec", base.video.codec);
    }
    if (root.contains("audio")) {
      const json& audio = root.at("audio");
      read_field(audio, "sample_rate_hz", base.audio.sample_rate_hz);
      read_field(audio, "channels", base.audio.channels);
      read_field(audio, "bitrate_kbps", base.audio.bitrate_kbps);
      read_field(audio, "frame_duration_ms", base.audio.frame_duration_ms);
      read_field(audio, "echo_cancellation", base.audio.echo_cancellation);
      read_field(audio, "noise_suppression", base.audio.noise_suppression);
      read_field(audio, "automatic_gain_control", base.audio.automatic_gain_control);
      read_field(audio, "input_device", base.audio.input_device);
      read_field(audio, "output_device", base.audio.output_device);
    }
    if (root.contains("network")) {
      const json& network = root.at("network");
      read_field(network, "signaling_url", base.network.signaling_url);
      read_field(network, "stun_servers", base.network.stun_servers);
      read_field(network, "turn_url", base.network.turn_url);
      read_field(network, "turn_username", base.network.turn_username);
      read_field(network, "turn_password", base.network.turn_password);
      read_field(network, "reconnect_initial_delay_ms", base.network.reconnect_initial_delay_ms);
      read_field(network, "reconnect_max_delay_ms", base.network.reconnect_max_delay_ms);
    }
    if (root.contains("logging")) {
      const json& logging = root.at("logging");
      read_field(logging, "level", base.logging.level);
      read_field(logging, "file_path", base.logging.file_path);
      read_field(logging, "log_to_console", base.logging.log_to_console);
      read_field(logging, "crash_directory", base.logging.crash_directory);
      read_field(logging, "crash_reports", base.logging.crash_reports);
    }
    if (root.contains("server")) {
      const json& server = root.at("server");
      read_field(server, "bind_address", base.server.bind_address);
      read_field(server, "port", base.server.port);
      read_field(server, "max_participants_per_room", base.server.max_participants_per_room);
      read_field(server, "heartbeat_interval_ms", base.server.heartbeat_interval_ms);
      read_field(server, "heartbeat_timeout_ms", base.server.heartbeat_timeout_ms);
      read_field(server, "users_file", base.server.users_file);
    }
  } catch (const json::exception& error) {
    return Result<Config>::failure("invalid_type", error.what());
  }

  return base;
}

Result<Config> load_from_file(const std::string& path) {
  Config config;

  std::ifstream input(path);
  if (input.is_open()) {
    std::ostringstream buffer;
    buffer << input.rdbuf();
    auto parsed = parse_json(buffer.str(), std::move(config));
    if (!parsed) {
      return parsed;
    }
    config = std::move(parsed).take();
  }

  apply_environment(config);

  if (const auto violation = validate(config)) {
    return Result<Config>::failure(*violation);
  }
  return config;
}

void apply_environment(Config& config) {
  apply_env_int("DV_VIDEO_WIDTH", config.video.width);
  apply_env_int("DV_VIDEO_HEIGHT", config.video.height);
  apply_env_int("DV_VIDEO_FPS", config.video.fps);
  apply_env_int("DV_VIDEO_MIN_BITRATE_KBPS", config.video.min_bitrate_kbps);
  apply_env_int("DV_VIDEO_MAX_BITRATE_KBPS", config.video.max_bitrate_kbps);
  apply_env_int("DV_VIDEO_FLOOR_BITRATE_KBPS", config.video.floor_bitrate_kbps);
  apply_env_string("DV_VIDEO_CODEC", config.video.codec);

  apply_env_int("DV_AUDIO_SAMPLE_RATE_HZ", config.audio.sample_rate_hz);
  apply_env_int("DV_AUDIO_CHANNELS", config.audio.channels);
  apply_env_int("DV_AUDIO_BITRATE_KBPS", config.audio.bitrate_kbps);
  apply_env_int("DV_AUDIO_FRAME_DURATION_MS", config.audio.frame_duration_ms);
  apply_env_bool("DV_AUDIO_ECHO_CANCELLATION", config.audio.echo_cancellation);
  apply_env_bool("DV_AUDIO_NOISE_SUPPRESSION", config.audio.noise_suppression);
  apply_env_bool("DV_AUDIO_AUTOMATIC_GAIN_CONTROL", config.audio.automatic_gain_control);
  apply_env_string("DV_AUDIO_INPUT_DEVICE", config.audio.input_device);
  apply_env_string("DV_AUDIO_OUTPUT_DEVICE", config.audio.output_device);

  apply_env_string("DV_SIGNALING_URL", config.network.signaling_url);
  apply_env_string("DV_TURN_URL", config.network.turn_url);
  apply_env_string("DV_TURN_USERNAME", config.network.turn_username);
  apply_env_string("DV_TURN_PASSWORD", config.network.turn_password);

  apply_env_string("DV_LOG_LEVEL", config.logging.level);
  apply_env_string("DV_LOG_FILE", config.logging.file_path);
  apply_env_string("DV_CRASH_DIRECTORY", config.logging.crash_directory);
  apply_env_bool("DV_CRASH_REPORTS", config.logging.crash_reports);

  apply_env_string("DV_SERVER_BIND_ADDRESS", config.server.bind_address);
  if (const auto raw = env("DV_SERVER_PORT")) {
    if (const auto parsed = parse_int(*raw); parsed && *parsed > 0 && *parsed <= 65535) {
      config.server.port = static_cast<std::uint16_t>(*parsed);
    }
  }
  apply_env_int("DV_SERVER_MAX_PARTICIPANTS", config.server.max_participants_per_room);
  apply_env_string("DV_SERVER_USERS_FILE", config.server.users_file);
}

// NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
Result<Config> load(int argc, const char* const argv[], UnknownOptions unknown) {
  // The configuration file path is the one setting that has to be read from
  // the command line before anything else can be loaded.
  std::string config_path = env("DV_CONFIG_FILE").value_or("");
  for (int i = 1; i < argc; ++i) {
    std::string_view key;
    std::string_view value;
    if (split_argument(argv[i], key, value) && key == "config") {
      config_path = std::string(value);
    }
  }

  Config config;
  if (!config_path.empty()) {
    std::ifstream input(config_path);
    if (!input.is_open()) {
      return Result<Config>::failure("config_file_not_found",
                                     "cannot open configuration file: " + config_path);
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    auto parsed = parse_json(buffer.str(), std::move(config));
    if (!parsed) {
      return parsed;
    }
    config = std::move(parsed).take();
  }

  apply_environment(config);

  for (int i = 1; i < argc; ++i) {
    const std::string_view argument(argv[i]);
    std::string_view key;
    std::string_view value;
    if (!split_argument(argument, key, value)) {
      // Anything that is not --key=value. Under Reject only a dash makes it an
      // error: a bare word is a positional argument, not a mistyped option.
      // --help and -h are let through because main answers them before the
      // configuration is loaded at all.
      if (unknown == UnknownOptions::Reject && argument.starts_with("-") && argument != "--help" &&
          argument != "-h") {
        return Result<Config>::failure(
            "invalid_argument",
            std::string(argument) + ": options take the --key=value form, with the value attached");
      }
      continue;
    }
    const std::string text(value);
    if (key == "config") {
      // Already read, before the file it names was loaded.
    } else if (key == "log-level") {
      config.logging.level = text;
    } else if (key == "log-file") {
      config.logging.file_path = text;
    } else if (key == "signaling-url") {
      config.network.signaling_url = text;
    } else if (key == "bind-address") {
      config.server.bind_address = text;
    } else if (key == "users-file") {
      config.server.users_file = text;
    } else if (key == "input-device") {
      config.audio.input_device = text;
    } else if (key == "output-device") {
      config.audio.output_device = text;
    } else if (key == "codec") {
      config.video.codec = text;
    } else if (key == "port") {
      if (const auto parsed = parse_int(text); parsed && *parsed > 0 && *parsed <= 65535) {
        config.server.port = static_cast<std::uint16_t>(*parsed);
      } else {
        return Result<Config>::failure(invalid_value("--port", "must be between 1 and 65535"));
      }
    } else if (key == "fps") {
      if (const auto parsed = parse_int(text)) {
        config.video.fps = *parsed;
      } else {
        return Result<Config>::failure(invalid_value("--fps", "must be an integer"));
      }
    } else if (key == "max-participants") {
      if (const auto parsed = parse_int(text)) {
        config.server.max_participants_per_room = *parsed;
      } else {
        return Result<Config>::failure(invalid_value("--max-participants", "must be an integer"));
      }
    } else if (unknown == UnknownOptions::Reject) {
      return Result<Config>::failure("unknown_option",
                                     "--" + std::string(key) + " is not an option");
    }
  }

  if (const auto violation = validate(config)) {
    return Result<Config>::failure(*violation);
  }
  return config;
}

std::optional<Error> validate(const Config& config) {
  if (config.video.width <= 0 || config.video.height <= 0) {
    return invalid_value("video.width/height", "must be positive");
  }
  if (config.video.width % 2 != 0 || config.video.height % 2 != 0) {
    return invalid_value("video.width/height", "must be even for H.264 chroma subsampling");
  }
  if (config.video.fps < 1 || config.video.fps > 120) {
    return invalid_value("video.fps", "must be between 1 and 120");
  }
  if (config.video.min_bitrate_kbps <= 0) {
    return invalid_value("video.min_bitrate_kbps", "must be positive");
  }
  if (config.video.max_bitrate_kbps < config.video.min_bitrate_kbps) {
    return invalid_value("video.max_bitrate_kbps", "must be at least min_bitrate_kbps");
  }
  if (config.video.floor_bitrate_kbps <= 0) {
    return invalid_value("video.floor_bitrate_kbps", "must be positive");
  }
  if (config.video.floor_bitrate_kbps > config.video.min_bitrate_kbps) {
    return invalid_value("video.floor_bitrate_kbps",
                         "must not be above min_bitrate_kbps, which is where the encoder starts");
  }

  static constexpr std::array<int, 5> kOpusSampleRates{8000, 12000, 16000, 24000, 48000};
  if (std::ranges::find(kOpusSampleRates, config.audio.sample_rate_hz) == kOpusSampleRates.end()) {
    return invalid_value("audio.sample_rate_hz", "must be a rate supported by Opus");
  }
  if (config.audio.channels < 1 || config.audio.channels > 2) {
    return invalid_value("audio.channels", "must be 1 or 2");
  }
  static constexpr std::array<int, 5> kOpusFrameDurations{5, 10, 20, 40, 60};
  if (std::ranges::find(kOpusFrameDurations, config.audio.frame_duration_ms) ==
      kOpusFrameDurations.end()) {
    return invalid_value("audio.frame_duration_ms", "must be 5, 10, 20, 40 or 60");
  }
  if (config.audio.bitrate_kbps < 6 || config.audio.bitrate_kbps > 510) {
    return invalid_value("audio.bitrate_kbps", "must be between 6 and 510");
  }

  if (config.network.signaling_url.empty()) {
    return invalid_value("network.signaling_url", "must not be empty");
  }
  if (!config.network.signaling_url.starts_with("ws://") &&
      !config.network.signaling_url.starts_with("wss://")) {
    return invalid_value("network.signaling_url", "must start with ws:// or wss://");
  }
  if (config.network.reconnect_initial_delay_ms <= 0 ||
      config.network.reconnect_max_delay_ms < config.network.reconnect_initial_delay_ms) {
    return invalid_value("network.reconnect delays", "max must be at least the initial delay");
  }

  if (config.server.port == 0) {
    return invalid_value("server.port", "must not be zero");
  }
  if (config.server.max_participants_per_room < 2) {
    return invalid_value("server.max_participants_per_room", "must be at least 2");
  }
  if (config.server.heartbeat_timeout_ms <= config.server.heartbeat_interval_ms) {
    return invalid_value("server.heartbeat_timeout_ms", "must exceed heartbeat_interval_ms");
  }

  return std::nullopt;
}

std::string to_json(const Config& config) {
  const json root = {{"video",
                      {{"width", config.video.width},
                       {"height", config.video.height},
                       {"fps", config.video.fps},
                       {"min_bitrate_kbps", config.video.min_bitrate_kbps},
                       {"max_bitrate_kbps", config.video.max_bitrate_kbps},
                       {"codec", config.video.codec}}},
                     {"audio",
                      {{"sample_rate_hz", config.audio.sample_rate_hz},
                       {"channels", config.audio.channels},
                       {"bitrate_kbps", config.audio.bitrate_kbps},
                       {"frame_duration_ms", config.audio.frame_duration_ms},
                       {"echo_cancellation", config.audio.echo_cancellation},
                       {"noise_suppression", config.audio.noise_suppression},
                       {"automatic_gain_control", config.audio.automatic_gain_control},
                       {"input_device", config.audio.input_device},
                       {"output_device", config.audio.output_device}}},
                     {"network",
                      {{"signaling_url", config.network.signaling_url},
                       {"stun_servers", config.network.stun_servers},
                       {"turn_url", config.network.turn_url},
                       {"turn_username", config.network.turn_username},
                       // The password is deliberately omitted. Section 17 of SPEC.md forbids
                       // writing credentials out in plain text.
                       {"reconnect_initial_delay_ms", config.network.reconnect_initial_delay_ms},
                       {"reconnect_max_delay_ms", config.network.reconnect_max_delay_ms}}},
                     {"logging",
                      {{"level", config.logging.level},
                       {"file_path", config.logging.file_path},
                       {"log_to_console", config.logging.log_to_console}}},
                     {"server",
                      {{"bind_address", config.server.bind_address},
                       {"port", config.server.port},
                       {"max_participants_per_room", config.server.max_participants_per_room},
                       {"heartbeat_interval_ms", config.server.heartbeat_interval_ms},
                       {"heartbeat_timeout_ms", config.server.heartbeat_timeout_ms},
                       {"users_file", config.server.users_file}}}};

  return root.dump(2);
}

}  // namespace dv::config
