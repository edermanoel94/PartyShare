#include <cstdlib>
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

}  // namespace
