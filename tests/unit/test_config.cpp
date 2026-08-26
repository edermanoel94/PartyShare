#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <utility>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

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

/// Points the per-user configuration directory at one that does not exist, for
/// the duration of a test.
///
/// Every test that calls load() without naming a file reads the cascade of
/// default_config_paths(), and the last entry of that cascade is the config.ini
/// of whoever is running the tests. Without this, the suite is testing the
/// machine as much as the code.
///
/// That is not hypothetical. A client built from a branch that had a setting
/// this one does not know wrote it to disk, and ten tests here began failing
/// with "no such setting as [video] auto_bitrate" - on a branch where nothing
/// about configuration had changed. The only way to get a green suite was to
/// delete the file, which is a fine thing to have to do to a machine and a
/// terrible thing to have to do to run tests.
///
/// A path that does not exist rather than an empty temporary directory: what
/// these tests want is no user configuration at all, and a file that is not
/// there is exactly that to the cascade. Forward slashes because
/// std::filesystem takes them on Windows too.
class ScopedEmptyConfigHome {
 public:
#ifdef _WIN32
  ScopedEmptyConfigHome() : base_("LOCALAPPDATA", "C:/dv-test-appdata") {}
#elif defined(__APPLE__)
  ScopedEmptyConfigHome() : base_("HOME", "/dv-test-home") {}
#else
  ScopedEmptyConfigHome() : base_("XDG_CONFIG_HOME", "/dv-test-config") {}
#endif

 private:
  ScopedEnv base_;
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
  // Off, and this default matters more than most: on would mean that every
  // config.ini already naming a bitrate has it overruled by the next upgrade,
  // silently, including the ones written for a link that cannot carry more.
  EXPECT_FALSE(config.video.auto_bitrate);
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
  const ScopedEmptyConfigHome home;
  const ScopedEnv env("DV_VIDEO_FPS", "24");
  const char* argv[] = {"dv_client", "--fps=15"};
  const auto result = dv::config::load(2, argv);
  ASSERT_TRUE(result.ok()) << result.error().message;
  EXPECT_EQ(result.value().video.fps, 15);
}

TEST(Config, IgnoresArgumentsThatAreNotInKeyValueForm) {
  const ScopedEmptyConfigHome home;
  const char* argv[] = {"dv_client", "--verbose", "positional", "-x"};
  const auto result = dv::config::load(4, argv);
  EXPECT_TRUE(result.ok()) << result.error().message;
}

TEST(Config, RejectingRefusesAnOptionNobodyDefined) {
  const ScopedEmptyConfigHome home;
  const char* argv[] = {"partyshare-server", "--prot=8080"};
  const auto result = dv::config::load(2, argv, dv::config::UnknownOptions::Reject);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code, "unknown_option");
}

TEST(Config, RejectingRefusesAnOptionWithTheValueDetached) {
  const ScopedEmptyConfigHome home;
  // The shape that let --help start a server instead of describing it.
  const char* argv[] = {"partyshare-server", "--port", "8080"};
  const auto result = dv::config::load(3, argv, dv::config::UnknownOptions::Reject);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code, "invalid_argument");
}

TEST(Config, RejectingStillLetsHelpThrough) {
  const ScopedEmptyConfigHome home;
  // main answers these before loading, so the parser must not refuse them.
  for (const char* flag : {"--help", "-h"}) {
    const char* argv[] = {"partyshare-server", flag};
    const auto result = dv::config::load(2, argv, dv::config::UnknownOptions::Reject);
    EXPECT_TRUE(result.ok()) << flag << ": " << result.error().message;
  }
}

TEST(Config, RejectingLeavesPositionalArgumentsAlone) {
  const ScopedEmptyConfigHome home;
  // Only a dash marks something as a mistyped option.
  const char* argv[] = {"partyshare-server", "positional"};
  const auto result = dv::config::load(2, argv, dv::config::UnknownOptions::Reject);
  EXPECT_TRUE(result.ok()) << result.error().message;
}

TEST(Config, RejectingKeepsReadingTheOptionsItKnows) {
  const ScopedEmptyConfigHome home;
  const char* argv[] = {"partyshare-server", "--port=9000"};
  const auto result = dv::config::load(2, argv, dv::config::UnknownOptions::Reject);
  ASSERT_TRUE(result.ok()) << result.error().message;
  EXPECT_EQ(result.value().server.port, 9000);
}

TEST(Config, IgnoringLetsThroughWhatQtWillReadItself) {
  const ScopedEmptyConfigHome home;
  // Why the client cannot reject. Qt spells its own options with the value
  // detached, as "-platform offscreen", which is the very shape Reject refuses:
  // turning it on here would refuse an argument that is not ours to judge.
  const char* argv[] = {"partyshare", "-platform", "offscreen", "--fps=15"};
  const auto result = dv::config::load(4, argv, dv::config::UnknownOptions::Ignore);
  ASSERT_TRUE(result.ok()) << result.error().message;
  EXPECT_EQ(result.value().video.fps, 15);
}

TEST(Config, ReportsAMissingConfigFile) {
  const ScopedEmptyConfigHome home;
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

TEST(Config, TheRoomChimeIsOnByDefaultAndCanBeTurnedOff) {
  // On by default: a cue nobody asked for is easier to turn off than a cue
  // nobody knows exists is to find.
  EXPECT_TRUE(Config{}.ui.room_sounds);

  const auto parsed = dv::config::parse_json(R"({"ui": {"room_sounds": false}})", Config{});
  ASSERT_TRUE(parsed.ok()) << parsed.error().message;
  EXPECT_FALSE(parsed.value().ui.room_sounds);
  // The section is the client's own, so reading it must not disturb anything
  // a call runs on.
  EXPECT_EQ(parsed.value().audio.input_device, Config{}.audio.input_device);
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
  original.video.auto_bitrate = true;
  original.video.fps = 24;
  original.audio.channels = 2;
  original.server.port = 9999;

  const auto reparsed = dv::config::parse_json(dv::config::to_json(original), Config{});
  ASSERT_TRUE(reparsed.ok()) << reparsed.error().message;
  EXPECT_TRUE(reparsed.value().video.auto_bitrate);
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
  const ScopedEmptyConfigHome home;
  const char* argv[] = {"partyshare-server", "--ice-port-range=50000-50100"};
  const auto result = dv::config::load(2, argv, dv::config::UnknownOptions::Reject);
  ASSERT_TRUE(result.ok()) << result.error().message;
  EXPECT_EQ(result.value().network.ice_port_range_begin, 50000);
  EXPECT_EQ(result.value().network.ice_port_range_end, 50100);
}

TEST(Config, RejectsAnIceRangeThatIsNotARange) {
  const ScopedEmptyConfigHome home;
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
      "[ui]\nroom_sounds = false\n"
      "[server]\nport = 9000\n"
      "[database]\nenabled = true\n",
      Config{});
  ASSERT_TRUE(parsed.ok()) << parsed.error().message;
  EXPECT_EQ(parsed.value().video.fps, 24);
  EXPECT_EQ(parsed.value().audio.channels, 2);
  EXPECT_EQ(parsed.value().network.ice_port_range_begin, 50000);
  EXPECT_EQ(parsed.value().logging.level, "debug");
  EXPECT_FALSE(parsed.value().ui.room_sounds);
  EXPECT_EQ(parsed.value().server.port, 9000);
  EXPECT_TRUE(parsed.value().database.enabled);
}

TEST(ConfigIni, ReadsTheAutomaticBitrateMode) {
  // The settings dialog writes this key, so a build that cannot read it back
  // is a dialog whose switch forgets itself between runs.
  const auto parsed = dv::config::parse_ini("[video]\nauto_bitrate = true\n", Config{});
  ASSERT_TRUE(parsed.ok()) << parsed.error().message;
  EXPECT_TRUE(parsed.value().video.auto_bitrate);

  const auto off = dv::config::parse_ini("[video]\nauto_bitrate = false\n", Config{});
  ASSERT_TRUE(off.ok()) << off.error().message;
  EXPECT_FALSE(off.value().video.auto_bitrate);
}

TEST(ConfigIni, ReadsWhetherTheRoomChimeIsOn) {
  // The settings dialog writes this key, so a build that cannot read it back
  // is a switch that forgets itself between runs - and one that forgets in the
  // direction of making noise, since the default is on.
  const auto off = dv::config::parse_ini("[ui]\nroom_sounds = false\n", Config{});
  ASSERT_TRUE(off.ok()) << off.error().message;
  EXPECT_FALSE(off.value().ui.room_sounds);

  const auto on = dv::config::parse_ini("[ui]\nroom_sounds = true\n", Config{});
  ASSERT_TRUE(on.ok()) << on.error().message;
  EXPECT_TRUE(on.value().ui.room_sounds);
}

TEST(ConfigIni, ReadsWhatAShareShouldCarryBesidesThePicture) {
  const auto parsed = dv::config::parse_ini("[screen_audio]\nmode = process\n", Config{});
  ASSERT_TRUE(parsed.ok()) << parsed.error().message;
  EXPECT_EQ(parsed.value().screen_audio.mode, "process");
}

TEST(Config, TheDefaultShareCarriesTheMachinesSound) {
  // Sharing a video and having nobody hear it is the surprise, not the other
  // way round. It is not a silent default either: the settings dialog shows the
  // choice before anything is shared.
  EXPECT_EQ(Config{}.screen_audio.mode, "system");
}

TEST(Config, ValidationRefusesAShareSoundModeNobodyImplements) {
  Config config;
  config.screen_audio.mode = "everything";
  const auto failure = dv::config::validate(config);
  ASSERT_TRUE(failure.has_value());
  EXPECT_EQ(failure->code, "invalid_value");
  EXPECT_NE(failure->message.find("screen_audio.mode"), std::string::npos);
}

TEST(Config, TheOpusCeilingDescribesWhatTheOfferAlreadyCarried) {
  // It was 48 and was applied to nothing: what went on the wire was
  // libdatachannel's own default of 96. Now that it reaches the offer, the
  // default has to be the number that was already true, or turning the setting
  // on would quietly halve every screen share's sound.
  EXPECT_EQ(Config{}.audio.bitrate_kbps, 96);
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
      {"[screen_audio]\nmodo = system\n", "a misspelled screen audio key"},
      {"[video]\nauto_bitrate = sometimes\n", "a mode that is not a boolean"},
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

// --- writing settings back ---------------------------------------------------

/// This process, for a temporary file name nothing else will pick.
long process_id() {
#ifdef _WIN32
  return static_cast<long>(_getpid());
#else
  return static_cast<long>(getpid());
#endif
}

/// A path in a temporary directory that no test file occupies yet, removed
/// afterwards along with anything save_ini_settings left beside it.
class ScopedPath {
 public:
  explicit ScopedPath(const std::string& extension = ".ini") {
    // The process id as well as the counter. ctest gives every test its own
    // process, so the counter starts at one in each of them, and the day
    // somebody runs the suite with -j two tests would be writing the same file.
    static int counter = 0;
    path_ = std::filesystem::temp_directory_path() / ("dv-save-" + std::to_string(process_id()) +
                                                      "-" + std::to_string(++counter) + extension);
    std::error_code ignored;
    std::filesystem::remove(path_, ignored);
  }

  ScopedPath(const ScopedPath&) = delete;
  ScopedPath& operator=(const ScopedPath&) = delete;
  ScopedPath(ScopedPath&&) = delete;
  ScopedPath& operator=(ScopedPath&&) = delete;

  ~ScopedPath() {
    std::error_code ignored;
    std::filesystem::remove(path_, ignored);
    std::filesystem::remove(std::filesystem::path(path_) += ".tmp", ignored);
  }

  [[nodiscard]] const std::filesystem::path& path() const { return path_; }

  void write(const std::string& text) const { std::ofstream(path_, std::ios::binary) << text; }

  [[nodiscard]] std::string read() const {
    std::ifstream input(path_, std::ios::binary);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
  }

 private:
  std::filesystem::path path_;
};

dv::config::IniSetting audio(const std::string& key, const std::string& value) {
  return dv::config::IniSetting{.section = "audio", .key = key, .value = value};
}

TEST(ConfigSave, CreatesTheFileWhenThereIsNoneYet) {
  const ScopedPath file;
  const auto written =
      dv::config::save_ini_settings(file.path(), {audio("input_device", "Yeti Nano")});
  ASSERT_TRUE(written.ok()) << written.error().message;

  const auto reloaded = dv::config::load_from_file(file.path().string());
  ASSERT_TRUE(reloaded.ok()) << reloaded.error().message;
  EXPECT_EQ(reloaded.value().audio.input_device, "Yeti Nano");
}

TEST(ConfigSave, LeavesEveryOtherLineExactlyWhereItWas) {
  // The point of writing settings one at a time rather than serialising the
  // whole Config: the file is edited by hand as well, and a save must not cost
  // somebody their comments or their ordering.
  const ScopedPath file;
  file.write(
      "; where the server is\n"
      "[network]\n"
      "signaling_url = ws://192.168.1.10:8080\n"
      "\n"
      "[audio]\n"
      "; input_device = Microfone (Realtek)\n"
      "\n"
      "[logging]\n"
      "level = debug\n");

  const auto written =
      dv::config::save_ini_settings(file.path(), {audio("input_device", "Yeti Nano")});
  ASSERT_TRUE(written.ok()) << written.error().message;

  EXPECT_EQ(file.read(),
            "; where the server is\n"
            "[network]\n"
            "signaling_url = ws://192.168.1.10:8080\n"
            "\n"
            "[audio]\n"
            "; input_device = Microfone (Realtek)\n"
            "input_device = Yeti Nano\n"
            "\n"
            "[logging]\n"
            "level = debug\n");
}

TEST(ConfigSave, RewritesAKeyWhereItStandsRatherThanAddingASecondOne) {
  const ScopedPath file;
  file.write("[audio]\ninput_device = Old\noutput_device = Speakers\n");

  const auto written = dv::config::save_ini_settings(file.path(), {audio("input_device", "New")});
  ASSERT_TRUE(written.ok()) << written.error().message;
  EXPECT_EQ(file.read(), "[audio]\ninput_device = New\noutput_device = Speakers\n");
}

TEST(ConfigSave, RewritesEveryCopyOfAKeyThatTheFileSetsTwice) {
  // The parser takes the last one. Rewriting only the first would leave the
  // stale value winning, and the save would look like it had done nothing.
  const ScopedPath file;
  file.write("[audio]\ninput_device = Old\ninput_device = AlsoOld\n");

  const auto written = dv::config::save_ini_settings(file.path(), {audio("input_device", "New")});
  ASSERT_TRUE(written.ok()) << written.error().message;
  EXPECT_EQ(file.read(), "[audio]\ninput_device = New\ninput_device = New\n");

  const auto reloaded = dv::config::load_from_file(file.path().string());
  ASSERT_TRUE(reloaded.ok()) << reloaded.error().message;
  EXPECT_EQ(reloaded.value().audio.input_device, "New");
}

TEST(ConfigSave, ACommentedLineIsNotTheKeyBeingPresent) {
  // A shipped config.ini is nothing but commented examples. Overwriting one in
  // place would leave the line commented, so the file would look changed and
  // behave exactly as before.
  const ScopedPath file;
  file.write("[audio]\n; input_device = Example\n");

  const auto written = dv::config::save_ini_settings(file.path(), {audio("input_device", "Real")});
  ASSERT_TRUE(written.ok()) << written.error().message;
  EXPECT_EQ(file.read(), "[audio]\n; input_device = Example\ninput_device = Real\n");
}

TEST(ConfigSave, AddsTheSectionWhenTheFileHasNone) {
  const ScopedPath file;
  file.write("[network]\nsignaling_url = ws://x:1\n");

  const auto written =
      dv::config::save_ini_settings(file.path(), {audio("output_device", "Fones")});
  ASSERT_TRUE(written.ok()) << written.error().message;
  EXPECT_EQ(file.read(), "[network]\nsignaling_url = ws://x:1\n\n[audio]\noutput_device = Fones\n");
}

TEST(ConfigSave, WritesSeveralSettingsInOnePass) {
  const ScopedPath file;
  const auto written = dv::config::save_ini_settings(
      file.path(), {audio("input_device", "Mic"), audio("output_device", "Speakers")});
  ASSERT_TRUE(written.ok()) << written.error().message;

  const auto reloaded = dv::config::load_from_file(file.path().string());
  ASSERT_TRUE(reloaded.ok()) << reloaded.error().message;
  EXPECT_EQ(reloaded.value().audio.input_device, "Mic");
  EXPECT_EQ(reloaded.value().audio.output_device, "Speakers");
}

TEST(ConfigSave, KeepsWindowsLineEndingsWhenTheFileAlreadyHadThem) {
  const ScopedPath file;
  file.write("[audio]\r\ninput_device = Old\r\n");

  const auto written = dv::config::save_ini_settings(file.path(), {audio("input_device", "New")});
  ASSERT_TRUE(written.ok()) << written.error().message;
  EXPECT_EQ(file.read(), "[audio]\r\ninput_device = New\r\n");
}

TEST(ConfigSave, RoundTripsAValueThatWouldNotSurviveUnquoted) {
  // Empty means the system's own device, and it has to be tellable from a
  // device whose name is a space.
  const ScopedPath file;
  const auto written = dv::config::save_ini_settings(
      file.path(), {audio("input_device", ""), audio("output_device", "  padded  ")});
  ASSERT_TRUE(written.ok()) << written.error().message;

  const auto reloaded = dv::config::load_from_file(file.path().string());
  ASSERT_TRUE(reloaded.ok()) << reloaded.error().message;
  EXPECT_EQ(reloaded.value().audio.input_device, "");
  EXPECT_EQ(reloaded.value().audio.output_device, "  padded  ");
}

TEST(ConfigSave, RoundTripsADeviceNameFullOfPunctuation) {
  const ScopedPath file;
  const std::string awkward = R"(Mic "Pro" #2; = [main])";
  const auto written = dv::config::save_ini_settings(file.path(), {audio("input_device", awkward)});
  ASSERT_TRUE(written.ok()) << written.error().message;

  const auto reloaded = dv::config::load_from_file(file.path().string());
  ASSERT_TRUE(reloaded.ok()) << reloaded.error().message;
  EXPECT_EQ(reloaded.value().audio.input_device, awkward);
}

TEST(ConfigSave, RefusesAValueWithALineBreakRatherThanWritingIt) {
  const ScopedPath file;
  const auto written =
      dv::config::save_ini_settings(file.path(), {audio("input_device", "two\nlines")});
  ASSERT_FALSE(written.ok());
  EXPECT_EQ(written.error().code, "invalid_value");
  EXPECT_FALSE(std::filesystem::exists(file.path()));
}

TEST(ConfigSave, RefusesToWriteAFileThatWouldNotParseBack) {
  // The guard that matters. An unknown key is a startup error, so a save that
  // wrote one would not lose a setting, it would stop the client from starting
  // and there would be nothing to connect the two.
  const ScopedPath file;
  const auto written = dv::config::save_ini_settings(
      file.path(),
      {dv::config::IniSetting{.section = "audio", .key = "no_such_setting", .value = "1"}});
  ASSERT_FALSE(written.ok());
  EXPECT_EQ(written.error().code, "config_write_failed");
  EXPECT_FALSE(std::filesystem::exists(file.path()));
}

TEST(ConfigSave, RefusesWhenThereIsNoPathToWriteTo) {
  const auto written = dv::config::save_ini_settings({}, {audio("input_device", "Mic")});
  ASSERT_FALSE(written.ok());
  EXPECT_EQ(written.error().code, "config_write_failed");
}

TEST(ConfigSave, CreatesTheDirectoriesOnTheWayToTheFile) {
  // First run on a machine that has never started the client: nothing under
  // the user's configuration directory exists yet, not even the directory.
  const ScopedPath root("");
  const std::filesystem::path file = root.path() / "nested" / "config.ini";

  const auto written = dv::config::save_ini_settings(file, {audio("input_device", "Mic")});
  ASSERT_TRUE(written.ok()) << written.error().message;
  EXPECT_TRUE(std::filesystem::exists(file));

  std::error_code ignored;
  std::filesystem::remove_all(root.path(), ignored);
}

TEST(ConfigSave, WritesTheBitrateRangeAsARange) {
  const ScopedPath file;
  const auto written = dv::config::save_ini_settings(
      file.path(),
      {dv::config::IniSetting{.section = "video", .key = "min_bitrate_kbps", .value = "2000"},
       dv::config::IniSetting{.section = "video", .key = "max_bitrate_kbps", .value = "4000"}});
  ASSERT_TRUE(written.ok()) << written.error().message;

  const auto reloaded = dv::config::load_from_file(file.path().string());
  ASSERT_TRUE(reloaded.ok()) << reloaded.error().message;
  EXPECT_EQ(reloaded.value().video.min_bitrate_kbps, 2000);
  EXPECT_EQ(reloaded.value().video.max_bitrate_kbps, 4000);
}

TEST(ConfigSave, RefusesAMinimumUnderTheFloorRatherThanWritingIt) {
  // The one that would have bitten. 200 kbps parses, and validate() refuses it
  // because the floor congestion control may squeeze the picture to defaults to
  // 300 and may not sit above the minimum. Written, it would be a client that
  // refuses to start with nothing on screen to connect it to the settings
  // dialog.
  const ScopedPath file;
  const auto written = dv::config::save_ini_settings(
      file.path(),
      {dv::config::IniSetting{.section = "video", .key = "min_bitrate_kbps", .value = "200"}});
  ASSERT_FALSE(written.ok());
  EXPECT_EQ(written.error().code, "config_write_failed");
  EXPECT_FALSE(std::filesystem::exists(file.path()));
}

TEST(ConfigSave, RefusesAMaximumUnderItsMinimum) {
  const ScopedPath file;
  file.write("[video]\nmin_bitrate_kbps = 3000\n");

  const auto written = dv::config::save_ini_settings(
      file.path(),
      {dv::config::IniSetting{.section = "video", .key = "max_bitrate_kbps", .value = "1000"}});
  ASSERT_FALSE(written.ok());
  EXPECT_EQ(written.error().code, "config_write_failed");

  // And the file it refused to change is the file it was.
  EXPECT_EQ(file.read(), "[video]\nmin_bitrate_kbps = 3000\n");
}

TEST(ConfigSave, AnythingItWritesIsSomethingLoadWouldAccept) {
  // The promise the whole thing rests on, stated once: whatever comes back ok
  // from here is a file the client starts on.
  const ScopedPath file;
  const auto written = dv::config::save_ini_settings(
      file.path(),
      {audio("input_device", "Yeti Nano"), audio("output_device", ""),
       dv::config::IniSetting{.section = "video", .key = "min_bitrate_kbps", .value = "1500"},
       dv::config::IniSetting{.section = "video", .key = "max_bitrate_kbps", .value = "3000"}});
  ASSERT_TRUE(written.ok()) << written.error().message;

  const auto reloaded = dv::config::load_from_file(file.path().string());
  ASSERT_TRUE(reloaded.ok()) << reloaded.error().message;
  EXPECT_FALSE(dv::config::validate(reloaded.value()).has_value());
}

TEST(ConfigSave, WritesToTheUsersOwnFileAndNotToTheMachines) {
  // Never the one beside the executable: on all three platforms that is a
  // directory the person running the client cannot write to.
#ifdef _WIN32
  const ScopedEnv base("LOCALAPPDATA", "C:/dv-test-appdata");
#elif defined(__APPLE__)
  const ScopedEnv base("HOME", "/dv-test-home");
#else
  const ScopedEnv base("XDG_CONFIG_HOME", "/dv-test-config");
#endif

  const std::filesystem::path mine = dv::config::user_config_file();
  ASSERT_FALSE(mine.empty());
  EXPECT_EQ(mine.filename().string(), "config.ini");
  EXPECT_NE(mine.string().find("dv-test-"), std::string::npos) << mine.string();

  // It is the last file of the cascade, which is the one that wins.
  EXPECT_EQ(mine, dv::config::default_config_paths().back());
}

}  // namespace
