#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <dv/core/result.hpp>

namespace dv::config {

/// Defaults come from sections 5.2, 6, 9 and 22 of SPEC.md.
struct VideoConfig {
  int width = 1280;
  int height = 720;
  int fps = 30;
  /// Where the encoder starts and what it aims for on a healthy link, section
  /// 6 of SPEC.md.
  int min_bitrate_kbps = 1500;
  int max_bitrate_kbps = 3000;
  /// The lowest congestion control may squeeze the screen share to before the
  /// picture stops being worth the bandwidth. Not a target: a link that cannot
  /// carry the minimum above has to be allowed to carry less, or it is simply
  /// flooded.
  int floor_bitrate_kbps = 300;
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
  /// The UDP range the SFU binds its ICE sockets in. Zero on both, the
  /// default, leaves the choice to the system's ephemeral range, which is what
  /// the server did before this setting existed.
  ///
  /// A range exists so a firewall can open something narrower than the whole
  /// ephemeral range. One port is taken per participant, so it has to hold
  /// server.max_participants_per_room times the number of rooms running at
  /// once. Note that 1024 to 65535 is the one range that means nothing:
  /// libdatachannel reads it as its own default and falls back to ephemeral.
  std::uint16_t ice_port_range_begin = 0;
  std::uint16_t ice_port_range_end = 0;
  int reconnect_initial_delay_ms = 500;
  int reconnect_max_delay_ms = 30000;
};

struct LoggingConfig {
  std::string level = "info";
  std::string file_path;
  bool log_to_console = true;
  /// Where a crash report is written. Empty means the platform's own place for
  /// state, which is what dv::diagnostics::default_crash_directory answers.
  std::string crash_directory;
  /// Crash reporting off is a legitimate choice for someone who does not want
  /// stack traces of their machine written to disk at all.
  bool crash_reports = true;
};

struct ServerConfig {
  std::string bind_address = "0.0.0.0";
  std::uint16_t port = 8080;
  int max_participants_per_room = 5;
  int heartbeat_interval_ms = 5000;
  int heartbeat_timeout_ms = 15000;
  /// Development only account list. See server/src/main.cpp.
  std::string users_file;
  /// "username:password". When set, the server creates or promotes that
  /// administrator and exits without listening. Not a configuration file
  /// setting in practice, but it travels with the rest so that one parser
  /// handles the whole command line.
  std::string create_admin;
};

/// Where the server persists accounts, rooms and the audit log.
///
/// Off by default, and the server then keeps all three in memory, which is
/// what it did before persistence existed. Turning it on without a reachable
/// MongoDB is a startup failure rather than a silent fallback: a server that
/// quietly forgets its accounts is worse than one that refuses to start.
struct DatabaseConfig {
  bool enabled = false;
  std::string uri = "mongodb://127.0.0.1:27017";
  std::string name = "partyshare";
  /// How long to wait for the database before giving up on an operation.
  ///
  /// Deliberately short. The store is called with the server's lock held, so a
  /// driver waiting on its own default of thirty seconds would not fail one
  /// login, it would hold every call on the server for half a minute.
  int timeout_ms = 2000;
};

struct Config {
  VideoConfig video;
  AudioConfig audio;
  NetworkConfig network;
  LoggingConfig logging;
  ServerConfig server;
  DatabaseConfig database;
};

/// What to do with a command line argument the parser does not recognise.
enum class UnknownOptions : std::uint8_t {
  /// Leave it alone. The client needs this: Qt reads its own options from the
  /// same command line, so an argument this parser does not know is not
  /// necessarily an argument nobody knows.
  Ignore,
  /// Refuse to start. A mistyped option is otherwise indistinguishable from a
  /// correct one, and the program runs on a default the operator never chose.
  Reject,
};

/// Precedence, from weakest to strongest: built-in defaults, configuration
/// file, environment variables, command line arguments.
///
/// The file is optional. A missing file is not an error, but a file that
/// exists and cannot be parsed is.
///
/// Options take the --key=value form. Under Reject, anything else that begins
/// with a dash is an error, including a bare --key: silently dropping one is
/// how --help came to start a server rather than describe it.
///
/// The array is what main() is handed, and there is no other shape for it.
/// NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
[[nodiscard]] Result<Config> load(int argc, const char* const argv[],
                                  UnknownOptions unknown = UnknownOptions::Ignore);

/// Same precedence, minus the command line. Useful in tests.
[[nodiscard]] Result<Config> load_from_file(const std::string& path);

[[nodiscard]] Result<Config> parse_json(const std::string& json_text, Config base);

/// Parses the INI form of the same settings.
///
/// Sections are the structs and keys are their fields, so [network] with
/// signaling_url is network.signaling_url, and nothing has to be learned twice.
/// Comments start with ; or #, a value may be wrapped in double quotes, and a
/// list - stun_servers is the only one - is separated by commas.
///
/// An unknown section or key is an error rather than a silence. A file nobody
/// validates is a file where a typed key does nothing at all, and "I put the
/// address in and it still connects to localhost" is the report that follows.
[[nodiscard]] Result<Config> parse_ini(const std::string& ini_text, Config base);

/// The files load() reads on its own, weakest first.
///
/// Next to the executable, then under the user's own configuration directory:
/// whoever installs a machine writes the first and it answers for everyone on
/// it, and a person overrides it in the second without being asked for an
/// administrator. Passing --config replaces both.
///
/// A path in the list is not a file that exists. Nothing here touches the disk.
[[nodiscard]] std::vector<std::filesystem::path> default_config_paths();

/// The config.ini a choice made in the interface belongs in.
///
/// The user's own, never the one beside the executable. That one is the
/// machine's, and on every platform this ships to it sits somewhere a program
/// running as the person at the keyboard cannot write: Program Files, an
/// /usr prefix, or inside a signed .app bundle where writing would break the
/// signature. It is also the wrong file on principle - which microphone Eder
/// picked is not an answer for every account on the machine.
///
/// Empty when the platform will not say where that directory is, which is the
/// same condition that leaves it out of default_config_paths().
[[nodiscard]] std::filesystem::path user_config_file();

/// One setting, spelled the way an INI file spells it.
struct IniSetting {
  std::string section;
  std::string key;
  std::string value;
};

/// Writes `settings` into the INI file at `path` and leaves the rest of it
/// exactly as it was.
///
/// Surgical rather than a serialization of the whole Config, for two reasons.
/// The file is written by hand as well, and rewriting it would take the
/// comments and the ordering with it. And a file holding every field freezes
/// today's defaults into it, so a default improved in a later version would
/// never reach anybody who had ever opened the settings dialog.
///
/// A key already in the file is rewritten where it stands. One that is not is
/// added at the end of its section, or under a new section header when the
/// file has no such section. A commented out line does not count as the key
/// being present: it is not what the parser reads, so overwriting it would
/// leave the setting looking applied while nothing had changed.
///
/// The result is parsed before it replaces anything, and nothing is written
/// when it does not parse. An unknown section or key is an error to parse_ini,
/// so without that check a mistyped key here would produce a config.ini that
/// stops the client from starting at all, and the person would have no reason
/// to connect the two.
///
/// Missing directories are created, and the replacement goes through a
/// temporary file so that an interrupted write cannot leave half a config.ini.
[[nodiscard]] Result<std::monostate> save_ini_settings(const std::filesystem::path& path,
                                                       const std::vector<IniSetting>& settings);

/// The files load() would read for this command line, in the order it reads
/// them: the cascade above, or the single file --config or DV_CONFIG_FILE
/// names, since that one replaces it.
///
/// Exists so that a program can say which files it read without working out
/// the answer a second time and getting it wrong. Reporting the cascade while
/// load() honoured an explicit --config is not a cosmetic mistake: it sends
/// whoever is reading the log to edit a file that is not being used.
///
/// The array is what main() is handed, and there is no other shape for it.
/// NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
[[nodiscard]] std::vector<std::filesystem::path> config_files(int argc, const char* const argv[]);

void apply_environment(Config& config);

/// Returns the first constraint violation found, or nothing when valid.
[[nodiscard]] std::optional<Error> validate(const Config& config);

/// Serializes back to JSON, so a running client can write out its effective
/// configuration.
[[nodiscard]] std::string to_json(const Config& config);

}  // namespace dv::config
