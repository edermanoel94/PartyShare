#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <utility>

#include <gtest/gtest.h>

#include <dv/config/config.hpp>

namespace {

using dv::config::Config;

/// Sets an environment variable for the duration of a test and restores the
/// previous state afterwards, so tests stay independent of each other.
class ScopedEnv {
 public:
  ScopedEnv(std::string name, const std::string& value) : name_(std::move(name)) {
    if (const char* previous = std::getenv(name_.c_str())) {
      previous_ = std::string(previous);
    }
#ifdef _WIN32
    _putenv_s(name_.c_str(), value.c_str());
#else
    setenv(name_.c_str(), value.c_str(), 1);
#endif
  }

  ScopedEnv(const ScopedEnv&) = delete;
  ScopedEnv& operator=(const ScopedEnv&) = delete;
  ScopedEnv(ScopedEnv&&) = delete;
  ScopedEnv& operator=(ScopedEnv&&) = delete;

  ~ScopedEnv() {
#ifdef _WIN32
    _putenv_s(name_.c_str(), previous_ ? previous_->c_str() : "");
#else
    if (previous_) {
      setenv(name_.c_str(), previous_->c_str(), 1);
    } else {
      unsetenv(name_.c_str());
    }
#endif
  }

 private:
  std::string name_;
  std::optional<std::string> previous_;
};

TEST(Config, DefaultsMatchTheSpec) {
  const Config config;
  EXPECT_EQ(config.video.width, 1280);
  EXPECT_EQ(config.video.height, 720);
  EXPECT_EQ(config.video.fps, 30);
  EXPECT_EQ(config.video.codec, "H264");
  EXPECT_EQ(config.audio.sample_rate_hz, 48000);
  EXPECT_EQ(config.audio.channels, 1);
  EXPECT_EQ(config.audio.frame_duration_ms, 20);
  EXPECT_EQ(config.server.max_participants_per_room, 5);
  EXPECT_TRUE(config.audio.echo_cancellation);
  EXPECT_TRUE(config.audio.noise_suppression);
  EXPECT_TRUE(config.audio.automatic_gain_control);
}

TEST(Config, DefaultsAreValid) {
  EXPECT_FALSE(dv::config::validate(Config{}).has_value());
}

TEST(Config, ParsesAPartialJsonObjectAndKeepsOtherDefaults) {
  const auto result = dv::config::parse_json(R"({"video": {"fps": 60}})", Config{});
  ASSERT_TRUE(result.ok()) << result.error().message;
  EXPECT_EQ(result.value().video.fps, 60);
  EXPECT_EQ(result.value().video.width, 1280);
}

TEST(Config, RejectsMalformedJson) {
  const auto result = dv::config::parse_json("{not json", Config{});
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code, "invalid_json");
}

TEST(Config, RejectsANonObjectRoot) {
  const auto result = dv::config::parse_json("[1, 2, 3]", Config{});
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code, "invalid_json");
}

TEST(Config, RejectsAFieldWithTheWrongType) {
  const auto result = dv::config::parse_json(R"({"video": {"fps": "thirty"}})", Config{});
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code, "invalid_type");
}

TEST(Config, EnvironmentOverridesTheFile) {
  const ScopedEnv env("DV_VIDEO_FPS", "24");
  auto config = dv::config::parse_json(R"({"video": {"fps": 60}})", Config{});
  ASSERT_TRUE(config.ok());
  Config effective = std::move(config).take();
  dv::config::apply_environment(effective);
  EXPECT_EQ(effective.video.fps, 24);
}

TEST(Config, CommandLineOverridesTheEnvironment) {
  const ScopedEnv env("DV_VIDEO_FPS", "24");
  const char* argv[] = {"dv_client", "--fps=15"};
  const auto result = dv::config::load(2, argv);
  ASSERT_TRUE(result.ok()) << result.error().message;
  EXPECT_EQ(result.value().video.fps, 15);
}

TEST(Config, IgnoresArgumentsThatAreNotInKeyValueForm) {
  const char* argv[] = {"dv_client", "--verbose", "positional", "-x"};
  const auto result = dv::config::load(4, argv);
  EXPECT_TRUE(result.ok()) << result.error().message;
}

TEST(Config, RejectingRefusesAnOptionNobodyDefined) {
  const char* argv[] = {"partyshare-server", "--prot=8080"};
  const auto result = dv::config::load(2, argv, dv::config::UnknownOptions::Reject);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code, "unknown_option");
}

TEST(Config, RejectingRefusesAnOptionWithTheValueDetached) {
  // The shape that let --help start a server instead of describing it.
  const char* argv[] = {"partyshare-server", "--port", "8080"};
  const auto result = dv::config::load(3, argv, dv::config::UnknownOptions::Reject);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code, "invalid_argument");
}

TEST(Config, RejectingStillLetsHelpThrough) {
  // main answers these before loading, so the parser must not refuse them.
  for (const char* flag : {"--help", "-h"}) {
    const char* argv[] = {"partyshare-server", flag};
    const auto result = dv::config::load(2, argv, dv::config::UnknownOptions::Reject);
    EXPECT_TRUE(result.ok()) << flag << ": " << result.error().message;
  }
}

TEST(Config, RejectingLeavesPositionalArgumentsAlone) {
  // Only a dash marks something as a mistyped option.
  const char* argv[] = {"partyshare-server", "positional"};
  const auto result = dv::config::load(2, argv, dv::config::UnknownOptions::Reject);
  EXPECT_TRUE(result.ok()) << result.error().message;
}

TEST(Config, RejectingKeepsReadingTheOptionsItKnows) {
  const char* argv[] = {"partyshare-server", "--port=9000"};
  const auto result = dv::config::load(2, argv, dv::config::UnknownOptions::Reject);
  ASSERT_TRUE(result.ok()) << result.error().message;
  EXPECT_EQ(result.value().server.port, 9000);
}

TEST(Config, IgnoringLetsThroughWhatQtWillReadItself) {
  // Why the client cannot reject. Qt spells its own options with the value
  // detached, as "-platform offscreen", which is the very shape Reject refuses:
  // turning it on here would refuse an argument that is not ours to judge.
  const char* argv[] = {"partyshare", "-platform", "offscreen", "--fps=15"};
  const auto result = dv::config::load(4, argv, dv::config::UnknownOptions::Ignore);
  ASSERT_TRUE(result.ok()) << result.error().message;
  EXPECT_EQ(result.value().video.fps, 15);
}

TEST(Config, ReportsAMissingConfigFile) {
  const char* argv[] = {"dv_client", "--config=/nonexistent/path/config.json"};
  const auto result = dv::config::load(2, argv);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code, "config_file_not_found");
}

TEST(Config, ValidationRejectsAnOddResolution) {
  Config config;
  config.video.width = 1281;
  const auto violation = dv::config::validate(config);
  ASSERT_TRUE(violation.has_value());
  EXPECT_EQ(violation->code, "invalid_value");
}

TEST(Config, ValidationRejectsABitrateRangeThatIsInverted) {
  Config config;
  config.video.min_bitrate_kbps = 3000;
  config.video.max_bitrate_kbps = 1500;
  EXPECT_TRUE(dv::config::validate(config).has_value());
}

TEST(Config, CrashReportingIsOnByDefaultAndCanBeTurnedOff) {
  // A stack trace of somebody's machine written to disk is a decision they are
  // entitled to make, so it is a setting rather than a fact.
  const Config defaults;
  EXPECT_TRUE(defaults.logging.crash_reports);
  EXPECT_TRUE(defaults.logging.crash_directory.empty())
      << "the default has to be empty so the platform's own state directory is used";
}

TEST(Config, ValidationRejectsAFloorAboveWhereTheEncoderStarts) {
  // The floor is what congestion control may squeeze down to, so a floor above
  // the starting point is a range with nothing in it.
  Config config;
  config.video.min_bitrate_kbps = 1500;
  config.video.floor_bitrate_kbps = 2000;
  EXPECT_TRUE(dv::config::validate(config).has_value());

  config.video.floor_bitrate_kbps = 0;
  EXPECT_TRUE(dv::config::validate(config).has_value());
}

TEST(Config, ValidationRejectsASampleRateOpusCannotUse) {
  Config config;
  config.audio.sample_rate_hz = 44100;
  EXPECT_TRUE(dv::config::validate(config).has_value());
}

TEST(Config, ValidationRejectsAFrameDurationOpusCannotUse) {
  Config config;
  config.audio.frame_duration_ms = 15;
  EXPECT_TRUE(dv::config::validate(config).has_value());
}

TEST(Config, ValidationRejectsANonWebSocketSignalingUrl) {
  Config config;
  config.network.signaling_url = "http://example.com";
  EXPECT_TRUE(dv::config::validate(config).has_value());
}

TEST(Config, ValidationRejectsAHeartbeatTimeoutBelowTheInterval) {
  Config config;
  config.server.heartbeat_interval_ms = 5000;
  config.server.heartbeat_timeout_ms = 1000;
  EXPECT_TRUE(dv::config::validate(config).has_value());
}

TEST(Config, SerializationOmitsTheTurnPassword) {
  Config config;
  config.network.turn_password = "super-secret";
  const std::string json = dv::config::to_json(config);
  EXPECT_EQ(json.find("super-secret"), std::string::npos);
  EXPECT_NE(json.find("signaling_url"), std::string::npos);
}

TEST(Config, SerializationRoundTrips) {
  Config original;
  original.video.fps = 24;
  original.audio.channels = 2;
  original.server.port = 9999;

  const auto reparsed = dv::config::parse_json(dv::config::to_json(original), Config{});
  ASSERT_TRUE(reparsed.ok()) << reparsed.error().message;
  EXPECT_EQ(reparsed.value().video.fps, 24);
  EXPECT_EQ(reparsed.value().audio.channels, 2);
  EXPECT_EQ(reparsed.value().server.port, 9999);
}

TEST(Config, TheIceRangeIsUnsetByDefault) {
  // Zero on both is what makes the SFU keep asking the system for an ephemeral
  // port, which is how it behaved before the range existed.
  const Config config;
  EXPECT_EQ(config.network.ice_port_range_begin, 0);
  EXPECT_EQ(config.network.ice_port_range_end, 0);
}

TEST(Config, ReadsTheIceRangeFromOneOption) {
  const char* argv[] = {"partyshare-server", "--ice-port-range=50000-50100"};
  const auto result = dv::config::load(2, argv, dv::config::UnknownOptions::Reject);
  ASSERT_TRUE(result.ok()) << result.error().message;
  EXPECT_EQ(result.value().network.ice_port_range_begin, 50000);
  EXPECT_EQ(result.value().network.ice_port_range_end, 50100);
}

TEST(Config, RejectsAnIceRangeThatIsNotARange) {
  for (const char* option :
       {"--ice-port-range=50000", "--ice-port-range=50000-", "--ice-port-range=-50100",
        "--ice-port-range=abc-def", "--ice-port-range=0-50100", "--ice-port-range=50000-70000"}) {
    const char* argv[] = {"partyshare-server", option};
    const auto result = dv::config::load(2, argv, dv::config::UnknownOptions::Reject);
    ASSERT_FALSE(result.ok()) << option << " was accepted";
    EXPECT_EQ(result.error().code, "invalid_value") << option;
  }
}

TEST(Config, ReadsTheIceRangeFromTheEnvironment) {
  const ScopedEnv begin("DV_ICE_PORT_RANGE_BEGIN", "40000");
  const ScopedEnv end("DV_ICE_PORT_RANGE_END", "40050");
  Config config;
  dv::config::apply_environment(config);
  EXPECT_EQ(config.network.ice_port_range_begin, 40000);
  EXPECT_EQ(config.network.ice_port_range_end, 40050);
}

TEST(Config, ValidationRejectsHalfAnIceRange) {
  // Half a range would quietly become no range at all, and the firewall rule
  // written against it would let nothing through.
  Config config;
  config.network.ice_port_range_begin = 50000;
  const auto violation = dv::config::validate(config);
  ASSERT_TRUE(violation.has_value());
  EXPECT_EQ(violation->code, "invalid_value");

  config.network.ice_port_range_begin = 0;
  config.network.ice_port_range_end = 50100;
  EXPECT_TRUE(dv::config::validate(config).has_value());
}

TEST(Config, ValidationRejectsAnIceRangeThatRunsBackwards) {
  Config config;
  config.network.ice_port_range_begin = 50100;
  config.network.ice_port_range_end = 50000;
  const auto violation = dv::config::validate(config);
  ASSERT_TRUE(violation.has_value());
  EXPECT_EQ(violation->code, "invalid_value");
}

TEST(Config, TheIceRangeSurvivesSerialization) {
  Config original;
  original.network.ice_port_range_begin = 50000;
  original.network.ice_port_range_end = 50100;

  const auto reparsed = dv::config::parse_json(dv::config::to_json(original), Config{});
  ASSERT_TRUE(reparsed.ok()) << reparsed.error().message;
  EXPECT_EQ(reparsed.value().network.ice_port_range_begin, 50000);
  EXPECT_EQ(reparsed.value().network.ice_port_range_end, 50100);
}

// --- INI ---------------------------------------------------------------------

/// Writes `text` to a uniquely named file and removes it afterwards.
class ScopedFile {
 public:
  ScopedFile(const std::string& extension, const std::string& text) {
    static int counter = 0;
    path_ = std::filesystem::temp_directory_path() /
            ("dv-config-" + std::to_string(++counter) + extension);
    std::ofstream(path_) << text;
  }

  ScopedFile(const ScopedFile&) = delete;
  ScopedFile& operator=(const ScopedFile&) = delete;
  ScopedFile(ScopedFile&&) = delete;
  ScopedFile& operator=(ScopedFile&&) = delete;

  ~ScopedFile() {
    std::error_code ignored;
    std::filesystem::remove(path_, ignored);
  }

  [[nodiscard]] std::string path() const { return path_.string(); }

 private:
  std::filesystem::path path_;
};

TEST(ConfigIni, ReadsTheServerAddress) {
  // The whole reason the format exists: somebody opens a file in Notepad and
  // writes down where the server is.
  const auto parsed = dv::config::parse_ini(
      "[network]\n"
      "signaling_url = ws://192.168.1.10:8080\n",
      Config{});
  ASSERT_TRUE(parsed.ok()) << parsed.error().message;
  EXPECT_EQ(parsed.value().network.signaling_url, "ws://192.168.1.10:8080");
}

TEST(ConfigIni, IgnoresCommentsBlankLinesAndCarriageReturns) {
  // The carriage returns are the point of half this test: the file is edited on
  // Windows, and on Linux every line would otherwise end in one.
  const auto parsed = dv::config::parse_ini(
      "; onde fica o servidor\r\n"
      "\r\n"
      "[network]\r\n"
      "# tambem um comentario\r\n"
      "   signaling_url   =   ws://10.0.0.5:9000   \r\n",
      Config{});
  ASSERT_TRUE(parsed.ok()) << parsed.error().message;
  EXPECT_EQ(parsed.value().network.signaling_url, "ws://10.0.0.5:9000");
}

TEST(ConfigIni, TakesAQuotedValue) {
  const auto parsed = dv::config::parse_ini(
      "[network]\n"
      "signaling_url = \"ws://192.168.1.10:8080\"\n",
      Config{});
  ASSERT_TRUE(parsed.ok()) << parsed.error().message;
  EXPECT_EQ(parsed.value().network.signaling_url, "ws://192.168.1.10:8080");
}

TEST(ConfigIni, SplitsAListOnCommas) {
  const auto parsed = dv::config::parse_ini(
      "[network]\n"
      "stun_servers = stun:a.example:3478, stun:b.example:3478\n",
      Config{});
  ASSERT_TRUE(parsed.ok()) << parsed.error().message;
  ASSERT_EQ(parsed.value().network.stun_servers.size(), 2U);
  EXPECT_EQ(parsed.value().network.stun_servers[0], "stun:a.example:3478");
  EXPECT_EQ(parsed.value().network.stun_servers[1], "stun:b.example:3478");
}

TEST(ConfigIni, ReachesEverySection) {
  const auto parsed = dv::config::parse_ini(
      "[video]\nfps = 24\n"
      "[audio]\nchannels = 2\n"
      "[network]\nice_port_range_begin = 50000\n"
      "[logging]\nlevel = debug\n"
      "[server]\nport = 9000\n"
      "[database]\nenabled = true\n",
      Config{});
  ASSERT_TRUE(parsed.ok()) << parsed.error().message;
  EXPECT_EQ(parsed.value().video.fps, 24);
  EXPECT_EQ(parsed.value().audio.channels, 2);
  EXPECT_EQ(parsed.value().network.ice_port_range_begin, 50000);
  EXPECT_EQ(parsed.value().logging.level, "debug");
  EXPECT_EQ(parsed.value().server.port, 9000);
  EXPECT_TRUE(parsed.value().database.enabled);
}

TEST(ConfigIni, RefusesWhatItCannotApply) {
  // Every one of these would otherwise be a line that looks applied and is not,
  // which is the failure this format exists to make impossible.
  const std::pair<const char*, const char*> cases[] = {
      {"[nowhere]\nkey = 1\n", "an unknown section"},
      {"[network]\nsignalling_url = ws://x:1\n", "a misspelled key"},
      {"[video]\nfps = many\n", "a number that is not one"},
      {"[audio]\nechho_cancellation = yes\n", "a misspelled boolean key"},
      {"[audio]\necho_cancellation = yeah\n", "a boolean that is not one"},
      {"[server]\nport = 70000\n", "a port out of range"},
      {"signaling_url = ws://x:1\n", "a setting under no section"},
      {"[network\nsignaling_url = ws://x:1\n", "an unterminated section header"},
      {"[network]\njust some words\n", "a line that is neither"},
      {"[network]\n = ws://x:1\n", "a nameless setting"},
  };
  for (const auto& [text, what] : cases) {
    const auto parsed = dv::config::parse_ini(text, Config{});
    ASSERT_FALSE(parsed.ok()) << what << " was accepted";
    EXPECT_EQ(parsed.error().code, "invalid_ini") << what;
    // The line number is what makes the file editable by a person.
    EXPECT_NE(parsed.error().message.find("line "), std::string::npos) << what;
  }
}

TEST(ConfigIni, LeavesAloneWhatItWasNotTold) {
  Config base;
  base.video.fps = 15;
  const auto parsed = dv::config::parse_ini("[network]\nsignaling_url = ws://x:1\n", base);
  ASSERT_TRUE(parsed.ok()) << parsed.error().message;
  EXPECT_EQ(parsed.value().video.fps, 15);
}

TEST(ConfigIni, LoadFromFilePicksTheParserByExtension) {
  const ScopedFile ini(".ini", "[network]\nsignaling_url = ws://192.168.1.10:8080\n");
  const auto from_ini = dv::config::load_from_file(ini.path());
  ASSERT_TRUE(from_ini.ok()) << from_ini.error().message;
  EXPECT_EQ(from_ini.value().network.signaling_url, "ws://192.168.1.10:8080");

  // JSON is the form that existed first, and it has to keep working.
  const ScopedFile json(".json", "{\"network\": {\"signaling_url\": \"ws://10.0.0.5:9000\"}}");
  const auto from_json = dv::config::load_from_file(json.path());
  ASSERT_TRUE(from_json.ok()) << from_json.error().message;
  EXPECT_EQ(from_json.value().network.signaling_url, "ws://10.0.0.5:9000");
}

TEST(ConfigIni, TheCascadeRunsFromTheExecutableToTheUser) {
  // Forward slashes on purpose: std::filesystem takes them on Windows too, and
  // a backslash here would only be an escape waiting to be got wrong.
#ifdef _WIN32
  const ScopedEnv base("LOCALAPPDATA", "C:/dv-test-appdata");
#elif defined(__APPLE__)
  const ScopedEnv base("HOME", "/dv-test-home");
#else
  const ScopedEnv base("XDG_CONFIG_HOME", "/dv-test-config");
#endif

  const auto paths = dv::config::default_config_paths();
  ASSERT_FALSE(paths.empty());
  for (const auto& path : paths) {
    EXPECT_EQ(path.filename().string(), "config.ini");
  }

  // The user's own comes last, which is what makes it the one that wins.
  const std::string last = paths.back().string();
  EXPECT_NE(last.find("dv-test-"), std::string::npos) << last;
  EXPECT_NE(last.find("partyshare"), std::string::npos) << last;

  // The machine's comes first, beside the executable running this.
  if (paths.size() > 1) {
    EXPECT_NE(paths.front(), paths.back());
  }
}

TEST(ConfigIni, TheNamedFileReplacesTheCascadeRatherThanJoiningIt) {
  // The log of a running client is built from this, so it has to answer what
  // load() will actually read. Naming files that were not read is what sends
  // somebody to edit the wrong one.
  const char* plain[] = {"partyshare"};
  EXPECT_EQ(dv::config::config_files(1, plain), dv::config::default_config_paths());

  const char* named[] = {"partyshare", "--config=/tmp/somewhere.ini"};
  const auto explicitly = dv::config::config_files(2, named);
  ASSERT_EQ(explicitly.size(), 1U);
  EXPECT_EQ(explicitly.front(), std::filesystem::path("/tmp/somewhere.ini"));

  const ScopedEnv from_env("DV_CONFIG_FILE", "/tmp/from-env.ini");
  const auto by_environment = dv::config::config_files(1, plain);
  ASSERT_EQ(by_environment.size(), 1U);
  EXPECT_EQ(by_environment.front(), std::filesystem::path("/tmp/from-env.ini"));

  // And the command line still beats the environment.
  const auto both = dv::config::config_files(2, named);
  ASSERT_EQ(both.size(), 1U);
  EXPECT_EQ(both.front(), std::filesystem::path("/tmp/somewhere.ini"));
}

}  // namespace
