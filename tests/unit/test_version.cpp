#include <optional>
#include <string>

#include <gtest/gtest.h>

#include <dv/core/version.hpp>

namespace {

using dv::core::is_update;
using dv::core::parse_version;
using dv::core::Version;

TEST(VersionParse, ReadsThreeNumbers) {
  const std::optional<Version> parsed = parse_version("0.1.41");
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->major, 0);
  EXPECT_EQ(parsed->minor, 1);
  EXPECT_EQ(parsed->patch, 41);
}

// The whole reason the parser exists rather than three calls to a number
// reader: what GitHub answers is the tag name, and the tag name has the v.
TEST(VersionParse, AcceptsTheTagsLeadingV) {
  EXPECT_EQ(parse_version("v0.1.41"), parse_version("0.1.41"));
  EXPECT_EQ(parse_version("V2.0.0"), parse_version("2.0.0"));
}

TEST(VersionParse, RefusesAnythingThatIsNotThreeNumbers) {
  EXPECT_FALSE(parse_version("").has_value());
  EXPECT_FALSE(parse_version("v").has_value());
  EXPECT_FALSE(parse_version("1").has_value());
  EXPECT_FALSE(parse_version("1.2").has_value());
  EXPECT_FALSE(parse_version("1.2.3.4").has_value());
  EXPECT_FALSE(parse_version("1..3").has_value());
  EXPECT_FALSE(parse_version("1.2.").has_value());
  EXPECT_FALSE(parse_version(".2.3").has_value());
  EXPECT_FALSE(parse_version("1.2.x").has_value());
  EXPECT_FALSE(parse_version("1.2.3 ").has_value());
  EXPECT_FALSE(parse_version("1.2.-3").has_value());
  // A pre-release is refused rather than read as its own number. This project
  // has never published one, and GitHub's "latest release" endpoint filters
  // them out anyway; reading 0.2.0-rc1 as 0.2.0 would be a client offering a
  // release candidate to everybody as though it were the release.
  EXPECT_FALSE(parse_version("0.2.0-rc1").has_value());
}

// The digits are counted rather than the value checked, so this is what proves
// a long string of them cannot wrap an int into a version that looks smaller
// than it is.
TEST(VersionParse, RefusesNumbersTooLongToHold) {
  EXPECT_TRUE(parse_version("999999999.0.0").has_value());
  EXPECT_FALSE(parse_version("9999999999.0.0").has_value());
}

TEST(VersionOrder, ComparesMajorThenMinorThenPatch) {
  EXPECT_LT((Version{.major = 0, .minor = 1, .patch = 41}),
            (Version{.major = 0, .minor = 1, .patch = 42}));
  EXPECT_LT((Version{.major = 0, .minor = 1, .patch = 99}),
            (Version{.major = 0, .minor = 2, .patch = 0}));
  EXPECT_LT((Version{.major = 0, .minor = 99, .patch = 99}),
            (Version{.major = 1, .minor = 0, .patch = 0}));
  // Which is the case a string comparison gets wrong, and the reason this is
  // three integers rather than the tag name.
  EXPECT_LT((Version{.major = 0, .minor = 2, .patch = 9}),
            (Version{.major = 0, .minor = 2, .patch = 10}));
}

TEST(VersionUpdate, IsOfferedOnlyWhenThePublishedOneIsNewer) {
  const Version running{.major = 0, .minor = 1, .patch = 41};
  EXPECT_TRUE(is_update(Version{.major = 0, .minor = 1, .patch = 42}, running));
  EXPECT_TRUE(is_update(Version{.major = 1, .minor = 0, .patch = 0}, running));
  EXPECT_FALSE(is_update(running, running));
}

// A build made from master after the last tag - every developer's, and every
// one from a pull request - is ahead of the newest release. Telling those
// people to install an older version is the one outcome worth a test of its
// own.
TEST(VersionUpdate, SaysNothingToABuildAheadOfTheLatestRelease) {
  const Version running{.major = 0, .minor = 2, .patch = 0};
  EXPECT_FALSE(is_update(Version{.major = 0, .minor = 1, .patch = 41}, running));
}

TEST(VersionRunning, IsWhatTheProjectVersionSays) {
  EXPECT_EQ(dv::core::running_version(), parse_version(DV_VERSION));
  EXPECT_EQ(dv::core::running_version().to_string(), std::string(DV_VERSION));
}

}  // namespace
