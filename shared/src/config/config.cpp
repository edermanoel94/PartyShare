#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string_view>
#include <system_error>

#include <nlohmann/json.hpp>

#include <dv/config/config.hpp>
#include <dv/models/room.hpp>

#ifdef _WIN32
// Before windows.h and not after, for the same reason crash_reporter.cpp says:
// the header defines min and max as macros.
#define NOMINMAX
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

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

/// Nothing when the text is not a boolean at all.
///
/// The fallback form below cannot say that, and a configuration file needs it:
/// `echo_cancellation = yeah` has to be an error rather than the default coming
/// back and the line reading as though it did something.
std::optional<bool> parse_bool_strict(std::string_view text) {
  if (text == "1" || text == "true" || text == "TRUE" || text == "yes" || text == "on") {
    return true;
  }
  if (text == "0" || text == "false" || text == "FALSE" || text == "no" || text == "off") {
    return false;
  }
  return std::nullopt;
}

bool parse_bool(std::string_view text, bool fallback) {
  return parse_bool_strict(text).value_or(fallback);
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

/// Zero is a legitimate value here, unlike for the port the server listens on:
/// it is how the ICE range says "leave it to the system".
void apply_env_port(const char* name, std::uint16_t& target) {
  if (const auto raw = env(name)) {
    if (const auto parsed = parse_int(*raw); parsed && *parsed >= 0 && *parsed <= 65535) {
      target = static_cast<std::uint16_t>(*parsed);
    }
  }
}

/// Replaces the credentials in a URI with asterisks, leaving the rest intact.
///
/// `mongodb://ana:secret@host/db` becomes `mongodb://***@host/db`. Used when
/// the configuration is written out, for the same reason turn_password is
/// omitted there: dumping the configuration must not be a way to read a
/// credential back.
std::string redact_uri(const std::string& uri) {
  const std::size_t scheme_end = uri.find("://");
  if (scheme_end == std::string::npos) {
    return uri;
  }
  const std::size_t host_start = scheme_end + 3;
  const std::size_t at = uri.find('@', host_start);
  if (at == std::string::npos) {
    return uri;
  }
  return uri.substr(0, host_start) + "***" + uri.substr(at);
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

// --- INI ---------------------------------------------------------------------

Error ini_error(int line, const std::string& reason) {
  return Error{.code = "invalid_ini", .message = "line " + std::to_string(line) + ": " + reason};
}

std::string_view trim(std::string_view text) {
  const auto is_space = [](char character) {
    return std::isspace(static_cast<unsigned char>(character)) != 0;
  };
  while (!text.empty() && is_space(text.front())) {
    text.remove_prefix(1);
  }
  while (!text.empty() && is_space(text.back())) {
    text.remove_suffix(1);
  }
  return text;
}

/// Drops one layer of double quotes.
///
/// Two callers want this for two reasons. A URL pasted out of the JSON form
/// arrives wearing quotes and refusing that would be pedantry rather than
/// validation. And a value written back out by save_ini_settings wears them
/// when it has to: see read_value below.
std::string_view unquote(std::string_view text) {
  if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
    text.remove_prefix(1);
    text.remove_suffix(1);
  }
  return text;
}

/// What a value on the right of an = actually says.
///
/// A bare value is trimmed, because `key = value` is how everybody writes one
/// and the spaces around it are layout. A quoted value is taken exactly as it
/// stands, because that is the only thing quoting can be for: whitespace at
/// either end, and a value that is nothing but whitespace. Trimming inside the
/// quotes as well - which this did until the settings dialog started writing
/// the file - leaves no spelling of "a device whose name ends in a space", and
/// a configuration format that cannot write back what it read is one that
/// quietly loses a setting now and then.
std::string_view read_value(std::string_view text) {
  const std::string_view trimmed = trim(text);
  const std::string_view unquoted = unquote(trimmed);
  return unquoted.size() == trimmed.size() ? trimmed : unquoted;
}

std::vector<std::string> split_list(std::string_view text) {
  std::vector<std::string> items;
  while (!text.empty()) {
    const std::size_t comma = text.find(',');
    const std::string_view item = trim(unquote(trim(text.substr(0, comma))));
    if (!item.empty()) {
      items.emplace_back(item);
    }
    if (comma == std::string_view::npos) {
      break;
    }
    text.remove_prefix(comma + 1);
  }
  return items;
}

/// Writes one key into `config`.
///
/// Returns nothing on success, and the reason otherwise. The two failures are
/// deliberately distinct in the message: a key nobody knows is a typo, and a
/// value nobody can read is a different mistake with a different fix.
std::optional<std::string> apply_ini_field(Config& config, std::string_view section,
                                           const std::string& key, const std::string& value) {
  const auto as_text = [&value](std::string& target) {
    target = value;
    return true;
  };
  const auto as_int = [&value](int& target) {
    const auto parsed = parse_int(value);
    if (!parsed) {
      return false;
    }
    target = *parsed;
    return true;
  };
  const auto as_port = [&value](std::uint16_t& target) {
    const auto parsed = parse_int(value);
    if (!parsed || *parsed < 0 || *parsed > 65535) {
      return false;
    }
    target = static_cast<std::uint16_t>(*parsed);
    return true;
  };
  const auto as_bool = [&value](bool& target) {
    const auto parsed = parse_bool_strict(value);
    if (!parsed) {
      return false;
    }
    target = *parsed;
    return true;
  };

  bool known = true;
  bool understood = true;

  if (section == "video") {
    if (key == "width") {
      understood = as_int(config.video.width);
    } else if (key == "height") {
      understood = as_int(config.video.height);
    } else if (key == "fps") {
      understood = as_int(config.video.fps);
    } else if (key == "auto_bitrate") {
      understood = as_bool(config.video.auto_bitrate);
    } else if (key == "min_bitrate_kbps") {
      understood = as_int(config.video.min_bitrate_kbps);
    } else if (key == "max_bitrate_kbps") {
      understood = as_int(config.video.max_bitrate_kbps);
    } else if (key == "floor_bitrate_kbps") {
      understood = as_int(config.video.floor_bitrate_kbps);
    } else if (key == "codec") {
      understood = as_text(config.video.codec);
    } else {
      known = false;
    }
  } else if (section == "screen_audio") {
    if (key == "mode") {
      understood = as_text(config.screen_audio.mode);
    } else if (key == "volume_percent") {
      understood = as_int(config.screen_audio.volume_percent);
    } else {
      known = false;
    }
  } else if (section == "audio") {
    if (key == "sample_rate_hz") {
      understood = as_int(config.audio.sample_rate_hz);
    } else if (key == "channels") {
      understood = as_int(config.audio.channels);
    } else if (key == "bitrate_kbps") {
      understood = as_int(config.audio.bitrate_kbps);
    } else if (key == "frame_duration_ms") {
      understood = as_int(config.audio.frame_duration_ms);
    } else if (key == "echo_cancellation") {
      understood = as_bool(config.audio.echo_cancellation);
    } else if (key == "noise_suppression") {
      understood = as_bool(config.audio.noise_suppression);
    } else if (key == "automatic_gain_control") {
      understood = as_bool(config.audio.automatic_gain_control);
    } else if (key == "input_device") {
      understood = as_text(config.audio.input_device);
    } else if (key == "output_device") {
      understood = as_text(config.audio.output_device);
    } else {
      known = false;
    }
  } else if (section == "network") {
    if (key == "signaling_url") {
      understood = as_text(config.network.signaling_url);
    } else if (key == "stun_servers") {
      config.network.stun_servers = split_list(value);
    } else if (key == "turn_url") {
      understood = as_text(config.network.turn_url);
    } else if (key == "turn_username") {
      understood = as_text(config.network.turn_username);
    } else if (key == "turn_password") {
      understood = as_text(config.network.turn_password);
    } else if (key == "ice_port_range_begin") {
      understood = as_port(config.network.ice_port_range_begin);
    } else if (key == "ice_port_range_end") {
      understood = as_port(config.network.ice_port_range_end);
    } else if (key == "reconnect_initial_delay_ms") {
      understood = as_int(config.network.reconnect_initial_delay_ms);
    } else if (key == "reconnect_max_delay_ms") {
      understood = as_int(config.network.reconnect_max_delay_ms);
    } else {
      known = false;
    }
  } else if (section == "logging") {
    if (key == "level") {
      understood = as_text(config.logging.level);
    } else if (key == "file_path") {
      understood = as_text(config.logging.file_path);
    } else if (key == "log_to_console") {
      understood = as_bool(config.logging.log_to_console);
    } else if (key == "crash_directory") {
      understood = as_text(config.logging.crash_directory);
    } else if (key == "crash_reports") {
      understood = as_bool(config.logging.crash_reports);
    } else {
      known = false;
    }
  } else if (section == "ui") {
    if (key == "room_sounds") {
      understood = as_bool(config.ui.room_sounds);
    } else {
      known = false;
    }
  } else if (section == "server") {
    if (key == "bind_address") {
      understood = as_text(config.server.bind_address);
    } else if (key == "port") {
      understood = as_port(config.server.port);
    } else if (key == "max_participants_per_room") {
      understood = as_int(config.server.max_participants_per_room);
    } else if (key == "heartbeat_interval_ms") {
      understood = as_int(config.server.heartbeat_interval_ms);
    } else if (key == "heartbeat_timeout_ms") {
      understood = as_int(config.server.heartbeat_timeout_ms);
    } else if (key == "users_file") {
      understood = as_text(config.server.users_file);
    } else {
      known = false;
    }
  } else if (section == "database") {
    if (key == "enabled") {
      understood = as_bool(config.database.enabled);
    } else if (key == "uri") {
      understood = as_text(config.database.uri);
    } else if (key == "name") {
      understood = as_text(config.database.name);
    } else if (key == "timeout_ms") {
      understood = as_int(config.database.timeout_ms);
    } else {
      known = false;
    }
  } else {
    known = false;
  }

  if (!known) {
    return "no such setting as [" + std::string(section) + "] " + key;
  }
  if (!understood) {
    return "'" + value + "' is not a value " + key + " can take";
  }
  return std::nullopt;
}

// --- where the files live ----------------------------------------------------

std::filesystem::path from_environment(const char* name) {
  const char* value = std::getenv(name);
  return value != nullptr && *value != '\0' ? std::filesystem::path(value)
                                            : std::filesystem::path();
}

/// The directory the running executable sits in.
///
/// Asked of the operating system rather than of argv[0] or the working
/// directory, and that is the whole point: a shortcut carries a working
/// directory of its own, so a file found relative to it is a file that depends
/// on how the program was launched. The start menu entry and the desktop icon
/// would then disagree about which server to connect to.
///
/// Empty when the answer cannot be had, which every caller treats as "there is
/// no file beside the executable".
std::filesystem::path executable_directory() {
#ifdef _WIN32
  std::wstring buffer(MAX_PATH, L'\0');
  for (;;) {
    const DWORD written =
        GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (written == 0) {
      return {};
    }
    // The call truncates rather than failing, and says so only by filling the
    // buffer exactly. A path longer than MAX_PATH is legal on a current
    // Windows, so growing until it fits is not a theoretical branch.
    if (written < buffer.size()) {
      buffer.resize(written);
      break;
    }
    if (buffer.size() > 32768) {
      return {};
    }
    buffer.resize(buffer.size() * 2);
  }
  return std::filesystem::path(buffer).parent_path();
#elif defined(__APPLE__)
  std::error_code failed;
  std::uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::string buffer(size, '\0');
  if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
    return {};
  }
  buffer.resize(std::string(buffer.c_str()).size());
  const std::filesystem::path resolved = std::filesystem::weakly_canonical(buffer, failed);
  return failed ? std::filesystem::path(buffer).parent_path() : resolved.parent_path();
#else
  std::error_code failed;
  const std::filesystem::path self = std::filesystem::read_symlink("/proc/self/exe", failed);
  return failed ? std::filesystem::path() : self.parent_path();
#endif
}

/// Where this user's own configuration belongs.
///
/// The same per-platform shape dv::diagnostics::default_crash_directory picks,
/// because two answers to "where does this program keep its files" is one too
/// many.
std::filesystem::path user_config_directory() {
#ifdef _WIN32
  std::filesystem::path base = from_environment("LOCALAPPDATA");
  if (base.empty()) {
    return {};
  }
  return base / "partyshare";
#elif defined(__APPLE__)
  const std::filesystem::path home = from_environment("HOME");
  if (home.empty()) {
    return {};
  }
  return home / "Library" / "Application Support" / "partyshare";
#else
  std::filesystem::path base = from_environment("XDG_CONFIG_HOME");
  if (base.empty()) {
    const std::filesystem::path home = from_environment("HOME");
    if (home.empty()) {
      return {};
    }
    base = home / ".config";
  }
  return base / "partyshare";
#endif
}

/// The file --config or DV_CONFIG_FILE names, or empty when neither does.
///
/// Pulled out of load() because config_files() has to give the same answer, and
/// the same question asked twice in two places is the same question answered
/// differently sooner or later.
///
/// The array is what main() is handed, and there is no other shape for it.
/// NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
std::string named_config_path(int argc, const char* const argv[]) {
  std::string path = env("DV_CONFIG_FILE").value_or("");
  for (int i = 1; i < argc; ++i) {
    std::string_view key;
    std::string_view value;
    if (split_argument(argv[i], key, value) && key == "config") {
      path = std::string(value);
    }
  }
  return path;
}

/// Reads one configuration file over `base`, choosing the parser by extension.
///
/// A missing file is an error only when somebody named it. The cascade is built
/// from paths that usually do not exist, and that is not the same event as
/// asking for a file by name and not finding it.
/// The whole of a text file, or nothing when it is not there.
///
/// Nothing and empty are different answers here: a file that does not exist is
/// one save_ini_settings creates from scratch, and an empty one is a file whose
/// content it has to preserve, which happens to be none.
std::optional<std::string> read_text_file(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input.is_open()) {
    return std::nullopt;
  }
  std::ostringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}

/// Splits text into lines, with the line terminators removed.
///
/// A carriage return goes with the newline, so a file written on Windows comes
/// apart into the same lines a file written on Linux does. Which of the two the
/// rebuilt file is joined back with is decided separately, by what it already
/// was.
std::vector<std::string> split_lines(std::string_view text) {
  std::vector<std::string> lines;
  while (!text.empty()) {
    const std::size_t newline = text.find('\n');
    std::string_view line = text.substr(0, newline);
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    lines.emplace_back(line);
    if (newline == std::string_view::npos) {
      break;
    }
    text.remove_prefix(newline + 1);
  }
  return lines;
}

/// The section a `[header]` line names, or nothing when the line is not one.
std::optional<std::string> section_header(std::string_view line) {
  const std::string_view trimmed = trim(line);
  if (trimmed.size() < 2 || trimmed.front() != '[' || trimmed.back() != ']') {
    return std::nullopt;
  }
  return std::string(trim(trimmed.substr(1, trimmed.size() - 2)));
}

/// The key a `key = value` line sets, or nothing for a comment, a blank line or
/// a header.
///
/// Comments are deliberately not keys. A shipped config.ini is mostly commented
/// out examples, and treating `; input_device =` as the setting being present
/// would overwrite the example and leave the line commented, so the file would
/// look changed and behave exactly as before.
std::optional<std::string> setting_key(std::string_view line) {
  const std::string_view trimmed = trim(line);
  if (trimmed.empty() || trimmed.front() == ';' || trimmed.front() == '#' ||
      trimmed.front() == '[') {
    return std::nullopt;
  }
  const std::size_t equals = trimmed.find('=');
  if (equals == std::string_view::npos) {
    return std::nullopt;
  }
  const std::string_view key = trim(trimmed.substr(0, equals));
  if (key.empty()) {
    return std::nullopt;
  }
  return std::string(key);
}

/// A value as it has to be spelled so that parse_ini reads back what went in.
///
/// Quotes only where they earn their place, because they are noise in a file
/// people edit by hand. Three cases need them: an empty value, which otherwise
/// leaves a line that looks unfinished; whitespace at either end, which trim
/// would eat; and a value that already opens and closes with a quote, which
/// unquote would strip. Inner quotes need nothing - unquote takes one layer.
std::string format_ini_value(const std::string& value) {
  const bool trimmed_away = trim(value) != value;
  const bool looks_quoted = value.size() >= 2 && value.front() == '"' && value.back() == '"';
  if (value.empty() || trimmed_away || looks_quoted) {
    return "\"" + value + "\"";
  }
  return value;
}

Result<Config> read_config_file(const std::filesystem::path& path, Config base, bool required) {
  std::ifstream input(path);
  if (!input.is_open()) {
    if (required) {
      return Result<Config>::failure("config_file_not_found",
                                     "cannot open configuration file: " + path.string());
    }
    return base;
  }
  std::ostringstream buffer;
  buffer << input.rdbuf();

  // By extension, so --config keeps taking the JSON it always took while the
  // discovered files are INI. One setting, two spellings, and the file itself
  // says which one it is.
  std::string extension = path.extension().string();
  std::ranges::transform(extension, extension.begin(), [](unsigned char character) {
    return static_cast<char>(std::tolower(character));
  });
  if (extension == ".ini") {
    return parse_ini(buffer.str(), std::move(base));
  }
  return parse_json(buffer.str(), std::move(base));
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
      read_field(video, "auto_bitrate", base.video.auto_bitrate);
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
    if (root.contains("screen_audio")) {
      const json& screen_audio = root.at("screen_audio");
      read_field(screen_audio, "mode", base.screen_audio.mode);
      read_field(screen_audio, "volume_percent", base.screen_audio.volume_percent);
    }
    if (root.contains("network")) {
      const json& network = root.at("network");
      read_field(network, "signaling_url", base.network.signaling_url);
      read_field(network, "stun_servers", base.network.stun_servers);
      read_field(network, "turn_url", base.network.turn_url);
      read_field(network, "turn_username", base.network.turn_username);
      read_field(network, "turn_password", base.network.turn_password);
      read_field(network, "ice_port_range_begin", base.network.ice_port_range_begin);
      read_field(network, "ice_port_range_end", base.network.ice_port_range_end);
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
    if (root.contains("ui")) {
      const json& ui = root.at("ui");
      read_field(ui, "room_sounds", base.ui.room_sounds);
    }
    if (root.contains("database")) {
      const json& database = root.at("database");
      read_field(database, "enabled", base.database.enabled);
      read_field(database, "uri", base.database.uri);
      read_field(database, "name", base.database.name);
      read_field(database, "timeout_ms", base.database.timeout_ms);
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

Result<Config> parse_ini(const std::string& ini_text, Config base) {
  std::istringstream input(ini_text);
  std::string raw;
  std::string section;
  int number = 0;

  while (std::getline(input, raw)) {
    ++number;
    // Trimming before anything else is also what makes a file written on
    // Windows readable on Linux: the carriage return is whitespace, and it
    // leaves with the rest of it.
    const std::string_view line = trim(raw);
    if (line.empty() || line.front() == ';' || line.front() == '#') {
      continue;
    }

    if (line.front() == '[') {
      if (line.back() != ']') {
        return Result<Config>::failure(ini_error(number, "a section header ends with ]"));
      }
      section = std::string(trim(line.substr(1, line.size() - 2)));
      if (section.empty()) {
        return Result<Config>::failure(ini_error(number, "a section needs a name"));
      }
      continue;
    }

    const std::size_t equals = line.find('=');
    if (equals == std::string_view::npos) {
      return Result<Config>::failure(ini_error(number, "expected [section], or key = value"));
    }
    // A setting above the first header would otherwise land nowhere and read as
    // though it had been applied.
    if (section.empty()) {
      return Result<Config>::failure(ini_error(number, "this setting is under no [section]"));
    }

    const std::string key(trim(line.substr(0, equals)));
    if (key.empty()) {
      return Result<Config>::failure(ini_error(number, "a setting needs a name"));
    }
    const std::string value(read_value(line.substr(equals + 1)));

    if (const auto reason = apply_ini_field(base, section, key, value)) {
      return Result<Config>::failure(ini_error(number, *reason));
    }
  }

  return base;
}

std::vector<std::filesystem::path> default_config_paths() {
  std::vector<std::filesystem::path> paths;
  if (const std::filesystem::path beside = executable_directory(); !beside.empty()) {
    paths.push_back(beside / "config.ini");
  }
  if (const std::filesystem::path mine = user_config_directory(); !mine.empty()) {
    paths.push_back(mine / "config.ini");
  }
  return paths;
}

std::filesystem::path user_config_file() {
  const std::filesystem::path mine = user_config_directory();
  if (mine.empty()) {
    return {};
  }
  return mine / "config.ini";
}

// NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
std::vector<std::filesystem::path> config_files(int argc, const char* const argv[]) {
  const std::string named = named_config_path(argc, argv);
  if (!named.empty()) {
    return {std::filesystem::path(named)};
  }
  return default_config_paths();
}

Result<Config> load_from_file(const std::string& path) {
  auto loaded = read_config_file(path, Config{}, false);
  if (!loaded) {
    return loaded;
  }
  Config config = std::move(loaded).take();

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
  apply_env_bool("DV_VIDEO_AUTO_BITRATE", config.video.auto_bitrate);
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
  apply_env_port("DV_ICE_PORT_RANGE_BEGIN", config.network.ice_port_range_begin);
  apply_env_port("DV_ICE_PORT_RANGE_END", config.network.ice_port_range_end);

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

  apply_env_bool("DV_DATABASE_ENABLED", config.database.enabled);
  apply_env_string("DV_DATABASE_URI", config.database.uri);
  apply_env_string("DV_DATABASE_NAME", config.database.name);
  apply_env_int("DV_DATABASE_TIMEOUT_MS", config.database.timeout_ms);
}

// NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
Result<Config> load(int argc, const char* const argv[], UnknownOptions unknown) {
  // The configuration file path is the one setting that has to be read from
  // the command line before anything else can be loaded.
  const std::string config_path = named_config_path(argc, argv);

  Config config;
  if (config_path.empty()) {
    // Nobody named a file, so the cascade applies: the one beside the
    // executable, which whoever installed the machine wrote and which answers
    // for every account on it, and then this user's own on top. Neither
    // existing is the ordinary case, and it leaves the built-in defaults.
    for (const std::filesystem::path& path : default_config_paths()) {
      auto loaded = read_config_file(path, std::move(config), false);
      if (!loaded) {
        return loaded;
      }
      config = std::move(loaded).take();
    }
  } else {
    // An explicit --config replaces the cascade rather than joining it. "Use
    // this file" is what it looks like it says, and a machine-wide setting
    // leaking into a run that named its own configuration is the kind of
    // surprise that costs an afternoon.
    auto loaded = read_config_file(config_path, std::move(config), true);
    if (!loaded) {
      return loaded;
    }
    config = std::move(loaded).take();
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
    } else if (key == "database-uri") {
      config.database.uri = text;
      // Naming a database is asking for one. Requiring --database-enabled as
      // well would be a second switch whose only job is to disagree with the
      // first, and a server started with a URI and no database is a server
      // whose operator will find out the hard way.
      config.database.enabled = true;
    } else if (key == "database-name") {
      config.database.name = text;
    } else if (key == "database-enabled") {
      config.database.enabled = parse_bool(text, true);
    } else if (key == "create-admin") {
      config.server.create_admin = text;
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
    } else if (key == "ice-port-range") {
      // One option rather than two, because a range is what an operator writes
      // into a firewall rule and half a range is never what anyone meant.
      const std::size_t dash = text.find('-');
      std::optional<int> begin;
      std::optional<int> end;
      if (dash != std::string::npos) {
        begin = parse_int(text.substr(0, dash));
        end = parse_int(text.substr(dash + 1));
      }
      if (!begin || !end || *begin < 1 || *begin > 65535 || *end < 1 || *end > 65535) {
        return Result<Config>::failure(invalid_value(
            "--ice-port-range", "must be BEGIN-END, both between 1 and 65535, as in 50000-50100"));
      }
      config.network.ice_port_range_begin = static_cast<std::uint16_t>(*begin);
      config.network.ice_port_range_end = static_cast<std::uint16_t>(*end);
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
  if (config.screen_audio.mode != "none" && config.screen_audio.mode != "system" &&
      config.screen_audio.mode != "process") {
    return invalid_value("screen_audio.mode", "must be none, system or process");
  }
  // The ceiling is audio::kMaxScreenVolumePercent, repeated here as a number
  // rather than included: this is the shared configuration library, and it is
  // linked by the server, which has no client audio code in it at all.
  if (config.screen_audio.volume_percent < 0 || config.screen_audio.volume_percent > 200) {
    return invalid_value("screen_audio.volume_percent", "must be between 0 and 200");
  }

  if (config.database.enabled) {
    if (config.database.uri.empty()) {
      return invalid_value("database.uri", "must not be empty when the database is enabled");
    }
    if (config.database.name.empty()) {
      return invalid_value("database.name", "must not be empty when the database is enabled");
    }
    if (config.database.timeout_ms <= 0) {
      return invalid_value("database.timeout_ms", "must be positive");
    }
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
  const bool range_begins = config.network.ice_port_range_begin != 0;
  const bool range_ends = config.network.ice_port_range_end != 0;
  if (range_begins != range_ends) {
    // Half a range would silently become no range at all, and the firewall rule
    // written against it would be wrong in a way nothing reports.
    return invalid_value("network.ice_port_range_begin/end",
                         "must both be set, or both left at zero for the ephemeral range");
  }
  if (range_begins && config.network.ice_port_range_begin > config.network.ice_port_range_end) {
    return invalid_value("network.ice_port_range_begin", "must not be above ice_port_range_end");
  }

  if (config.server.port == 0) {
    return invalid_value("server.port", "must not be zero");
  }
  if (config.server.max_participants_per_room < models::kMinRoomCapacity) {
    return invalid_value("server.max_participants_per_room",
                         "must be at least " + std::to_string(models::kMinRoomCapacity));
  }
  if (config.server.max_participants_per_room > models::kMaxRoomCapacity) {
    return invalid_value("server.max_participants_per_room",
                         "must be at most " + std::to_string(models::kMaxRoomCapacity));
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
                       {"auto_bitrate", config.video.auto_bitrate},
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
                     {"screen_audio",
                      {{"mode", config.screen_audio.mode},
                       {"volume_percent", config.screen_audio.volume_percent}}},
                     {"network",
                      {{"signaling_url", config.network.signaling_url},
                       {"stun_servers", config.network.stun_servers},
                       {"turn_url", config.network.turn_url},
                       {"turn_username", config.network.turn_username},
                       {"ice_port_range_begin", config.network.ice_port_range_begin},
                       {"ice_port_range_end", config.network.ice_port_range_end},
                       // The password is deliberately omitted. Section 17 of SPEC.md forbids
                       // writing credentials out in plain text.
                       {"reconnect_initial_delay_ms", config.network.reconnect_initial_delay_ms},
                       {"reconnect_max_delay_ms", config.network.reconnect_max_delay_ms}}},
                     {"logging",
                      {{"level", config.logging.level},
                       {"file_path", config.logging.file_path},
                       {"log_to_console", config.logging.log_to_console}}},
                     {"ui", {{"room_sounds", config.ui.room_sounds}}},
                     {"server",
                      {{"bind_address", config.server.bind_address},
                       {"port", config.server.port},
                       {"max_participants_per_room", config.server.max_participants_per_room},
                       {"heartbeat_interval_ms", config.server.heartbeat_interval_ms},
                       {"heartbeat_timeout_ms", config.server.heartbeat_timeout_ms},
                       {"users_file", config.server.users_file}}},
                     {"database",
                      {{"enabled", config.database.enabled},
                       // The URI is written out without its credentials, if it
                       // carried any: mongodb://user:password@host is a
                       // perfectly ordinary URI and dumping the configuration
                       // must not be a way to read the password out of it.
                       {"uri", redact_uri(config.database.uri)},
                       {"name", config.database.name},
                       {"timeout_ms", config.database.timeout_ms}}}};

  return root.dump(2);
}

Result<std::monostate> save_ini_settings(const std::filesystem::path& path,
                                         const std::vector<IniSetting>& settings) {
  using Written = Result<std::monostate>;

  if (path.empty()) {
    return Written::failure("config_write_failed",
                            "this system does not say where a user's configuration belongs");
  }
  for (const IniSetting& setting : settings) {
    if (setting.section.empty() || setting.key.empty()) {
      return Written::failure("invalid_value", "a setting needs both a section and a key");
    }
    // An INI value is the rest of its line, so there is no spelling of one that
    // carries a line break. Caught here rather than written out and discovered
    // on the next start, when the file no longer parses.
    if (setting.value.find('\n') != std::string::npos ||
        setting.value.find('\r') != std::string::npos) {
      return Written::failure("invalid_value",
                              setting.section + "." + setting.key + " cannot hold a line break");
    }
  }

  const std::string original = read_text_file(path).value_or(std::string{});
  // Whatever the file already is. Rewriting a Windows file with Unix endings
  // turns one changed setting into a diff of every line for whoever is keeping
  // the thing under version control.
  const std::string_view line_ending = original.find("\r\n") != std::string::npos ? "\r\n" : "\n";
  std::vector<std::string> lines = split_lines(original);

  const auto spell = [](const IniSetting& setting) {
    return setting.key + " = " + format_ini_value(setting.value);
  };

  // First pass: keys the file already sets, rewritten where they stand.
  //
  // Every occurrence and not the first, because a file may set the same key
  // twice and the parser takes the last. Rewriting one of them and leaving the
  // other is how a save appears to do nothing.
  std::vector<bool> found(settings.size(), false);
  {
    std::string section;
    for (std::string& line : lines) {
      if (const auto header = section_header(line)) {
        section = *header;
        continue;
      }
      const auto key = setting_key(line);
      if (!key) {
        continue;
      }
      for (std::size_t index = 0; index < settings.size(); ++index) {
        if (settings[index].section == section && settings[index].key == *key) {
          line = spell(settings[index]);
          found[index] = true;
        }
      }
    }
  }

  // Where a key that is not in the file yet has to go: the end of its section.
  //
  // The last line of the section that has something on it, rather than the
  // line before the next header. A key put after the blank line that separates
  // two sections still belongs to the earlier one as far as the parser is
  // concerned, and to a reader it looks like it belongs to the later one.
  const auto end_of_section = [&lines](const std::string& wanted) -> std::optional<std::size_t> {
    std::optional<std::size_t> last;
    std::string section;
    for (std::size_t index = 0; index < lines.size(); ++index) {
      if (const auto header = section_header(lines[index])) {
        section = *header;
        // The header itself, so that a section with nothing under it yet is
        // still a section this can add to.
        if (section == wanted) {
          last = index;
        }
        continue;
      }
      if (section == wanted && !trim(lines[index]).empty()) {
        last = index;
      }
    }
    return last;
  };

  for (std::size_t index = 0; index < settings.size(); ++index) {
    if (found[index]) {
      continue;
    }
    if (const auto after = end_of_section(settings[index].section)) {
      lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(*after) + 1, spell(settings[index]));
      continue;
    }
    if (!lines.empty() && !trim(lines.back()).empty()) {
      lines.emplace_back();
    }
    lines.push_back("[" + settings[index].section + "]");
    lines.push_back(spell(settings[index]));
  }

  std::string rebuilt;
  for (const std::string& line : lines) {
    rebuilt += line;
    rebuilt += line_ending;
  }

  // The check that matters, and the reason this is worth doing at all: what is
  // about to be written goes through everything load() would put it through. A
  // config.ini that does not survive that is not a setting that failed to save,
  // it is a client that will not start, and nobody would connect that to having
  // changed their microphone.
  //
  // Both halves are needed, and parsing alone is not enough. An unknown key is
  // the parser's to catch; `min_bitrate_kbps = 200` parses perfectly and is
  // refused by validate(), because the floor congestion control may squeeze the
  // picture to defaults to 300 and is not allowed to sit above the minimum.
  // That is a real way for a settings dialog to write a file nobody can start.
  const auto reparsed = parse_ini(rebuilt, Config{});
  if (!reparsed) {
    return Written::failure(
        "config_write_failed",
        "refusing to write a configuration that cannot be read back: " + reparsed.error().message);
  }
  // Over the built-in defaults, so this file is judged on its own rather than
  // as part of the cascade. That is the strict reading, and a file that only
  // holds together alongside the machine's is refused here. The strictness errs
  // in the safe direction: it fails visibly, in front of somebody who has just
  // changed a setting, rather than at the next startup with nothing to go on.
  if (const auto violation = validate(reparsed.value())) {
    return Written::failure(
        "config_write_failed",
        "refusing to write a configuration the client would reject: " + violation->message);
  }

  std::error_code failed;
  if (const std::filesystem::path parent = path.parent_path(); !parent.empty()) {
    std::filesystem::create_directories(parent, failed);
    if (failed) {
      return Written::failure("config_write_failed",
                              "cannot create " + parent.string() + ": " + failed.message());
    }
  }

  // Beside the file it replaces rather than in the system temporary directory:
  // a rename is only atomic within one filesystem, and those two are routinely
  // not the same one.
  std::filesystem::path temporary = path;
  temporary += ".tmp";
  {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
      return Written::failure("config_write_failed", "cannot write " + temporary.string());
    }
    output << rebuilt;
    output.flush();
    if (!output) {
      std::error_code ignored;
      std::filesystem::remove(temporary, ignored);
      return Written::failure("config_write_failed", "cannot write " + temporary.string());
    }
  }

  std::filesystem::rename(temporary, path, failed);
  if (failed) {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    return Written::failure("config_write_failed",
                            "cannot replace " + path.string() + ": " + failed.message());
  }
  return std::monostate{};
}

}  // namespace dv::config
