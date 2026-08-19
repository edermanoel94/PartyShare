#include <gtest/gtest.h>

#include <dv/logging/logger.hpp>

namespace {

using dv::log::Level;

TEST(Logging, ParsesEveryLevelName) {
  EXPECT_EQ(dv::log::level_from_string("trace"), Level::Trace);
  EXPECT_EQ(dv::log::level_from_string("debug"), Level::Debug);
  EXPECT_EQ(dv::log::level_from_string("info"), Level::Info);
  EXPECT_EQ(dv::log::level_from_string("warn"), Level::Warn);
  EXPECT_EQ(dv::log::level_from_string("error"), Level::Error);
  EXPECT_EQ(dv::log::level_from_string("fatal"), Level::Fatal);
  EXPECT_EQ(dv::log::level_from_string("off"), Level::Off);
}

TEST(Logging, ParsingIsCaseInsensitive) {
  EXPECT_EQ(dv::log::level_from_string("INFO"), Level::Info);
  EXPECT_EQ(dv::log::level_from_string("WaRn"), Level::Warn);
}

TEST(Logging, AcceptsCommonAliases) {
  EXPECT_EQ(dv::log::level_from_string("warning"), Level::Warn);
  EXPECT_EQ(dv::log::level_from_string("critical"), Level::Fatal);
  EXPECT_EQ(dv::log::level_from_string("none"), Level::Off);
}

TEST(Logging, UnknownLevelUsesTheFallback) {
  EXPECT_EQ(dv::log::level_from_string("verbose", Level::Warn), Level::Warn);
  EXPECT_EQ(dv::log::level_from_string("", Level::Error), Level::Error);
}

TEST(Logging, RoundTripsThroughTheNameTable) {
  for (const Level level : {Level::Trace, Level::Debug, Level::Info, Level::Warn, Level::Error,
                            Level::Fatal, Level::Off}) {
    EXPECT_EQ(dv::log::level_from_string(dv::log::to_string(level)), level);
  }
}

TEST(Logging, InitAndShutdownAreSafe) {
  dv::log::init({.level = Level::Off, .file_path = {}, .log_to_console = false});
  DV_LOG_INFO("this must not crash and must not print");
  dv::log::shutdown();
}

TEST(Logging, LoggingAfterShutdownIsSafe) {
  // Threads outlive the shutdown call in practice: media callbacks and worker
  // threads keep logging while the process is on its way out.
  dv::log::init({.level = Level::Info, .file_path = {}, .log_to_console = false});
  dv::log::shutdown();

  DV_LOG_ERROR("this arrives after shutdown and must be discarded, not crash");
  DV_LOG_INFO("so does this one");
}

}  // namespace
