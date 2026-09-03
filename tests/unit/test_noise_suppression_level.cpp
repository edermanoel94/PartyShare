// The spelling of the noise suppression level, between the configuration and
// the media layer.
//
// Small, and pinned anyway: the settings dialog writes what to_string says and
// the client reads it back through parse_noise_suppression_level, so the two
// have to agree with each other and with what config.cpp accepts, or a level
// chosen in the dialog comes back as a startup error.
#include <optional>
#include <string_view>

#include <gtest/gtest.h>

#include "media/media_session.hpp"

namespace {

using dv::client::media::NoiseSuppressionLevel;
using dv::client::media::parse_noise_suppression_level;
using dv::client::media::to_string;

TEST(NoiseSuppressionLevel, EveryLevelRoundTripsThroughItsSpelling) {
  for (const NoiseSuppressionLevel level :
       {NoiseSuppressionLevel::Low, NoiseSuppressionLevel::Moderate, NoiseSuppressionLevel::High,
        NoiseSuppressionLevel::VeryHigh}) {
    EXPECT_EQ(parse_noise_suppression_level(to_string(level)), level);
  }
}

TEST(NoiseSuppressionLevel, TheSpellingIsTheConfigurationsOwn) {
  // These are the words docs/03-configuration.md documents and config.cpp
  // accepts; changing one here without the other is a level nobody can set.
  EXPECT_EQ(to_string(NoiseSuppressionLevel::Low), "low");
  EXPECT_EQ(to_string(NoiseSuppressionLevel::Moderate), "moderate");
  EXPECT_EQ(to_string(NoiseSuppressionLevel::High), "high");
  EXPECT_EQ(to_string(NoiseSuppressionLevel::VeryHigh), "very_high");
}

TEST(NoiseSuppressionLevel, AWordThatIsNotALevelIsNothing) {
  EXPECT_EQ(parse_noise_suppression_level("loud"), std::nullopt);
  EXPECT_EQ(parse_noise_suppression_level("High"), std::nullopt);
  EXPECT_EQ(parse_noise_suppression_level(""), std::nullopt);
}

}  // namespace
