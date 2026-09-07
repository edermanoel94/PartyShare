// The envelope of the voice gate, exercised by saying "voice" and "silence"
// rather than by playing either.
//
// Every block fed in is a block of ones, so that what comes out is the gain
// itself: a block that comes back as ones was let through untouched, a block
// of zeros was silenced, and anything else is a ramp, which is the shape the
// two edges have to have.
#include <algorithm>
#include <cstddef>
#include <optional>
#include <vector>

#include <gtest/gtest.h>

#include "audio/voice_gate.hpp"
#include "media/media_session.hpp"

namespace {

using dv::client::audio::VoiceGate;
using dv::client::audio::VoiceGateTiming;

constexpr int kRate = 48000;
constexpr std::size_t kBlock = 480;
// The defaults, in blocks: 300 ms of hold and 100 ms of release.
constexpr int kHoldBlocks = 30;
constexpr int kReleaseBlocks = 10;

std::vector<float> run(VoiceGate& gate, bool voiced) {
  std::vector<float> block(kBlock, 1.0F);
  float* channel = block.data();
  gate.apply(voiced, &channel, 1, kBlock);
  return block;
}

bool untouched(const std::vector<float>& block) {
  return std::all_of(block.begin(), block.end(), [](float sample) { return sample == 1.0F; });
}

bool silent(const std::vector<float>& block) {
  return std::all_of(block.begin(), block.end(), [](float sample) { return sample == 0.0F; });
}

bool never_rises(const std::vector<float>& block) {
  return std::is_sorted(block.rbegin(), block.rend());
}

bool never_falls(const std::vector<float>& block) {
  return std::is_sorted(block.begin(), block.end());
}

TEST(VoiceGate, StartsOpenAndStaysOpenForAHold) {
  // Nobody has said anything yet, and the gate is still open: the first
  // thing heard after a reset is the microphone, not a closed gate waiting
  // for permission.
  VoiceGate gate;
  gate.reset(kRate);
  EXPECT_TRUE(gate.open());

  for (int block = 0; block < kHoldBlocks - 1; ++block) {
    EXPECT_TRUE(untouched(run(gate, false))) << "block " << block;
    EXPECT_TRUE(gate.open()) << "block " << block;
  }
}

TEST(VoiceGate, ClosesWithARampOnceTheHoldRunsOut) {
  VoiceGate gate;
  gate.reset(kRate);
  for (int block = 0; block < kHoldBlocks - 1; ++block) {
    run(gate, false);
  }

  // The block that ends the hold begins the release: it starts under one,
  // goes down and does not reach zero, because the release is ten of these.
  const std::vector<float> first = run(gate, false);
  EXPECT_FALSE(gate.open());
  EXPECT_LT(first.front(), 1.0F);
  EXPECT_GT(first.back(), 0.0F);
  EXPECT_TRUE(never_rises(first));

  for (int block = 1; block < kReleaseBlocks; ++block) {
    EXPECT_TRUE(never_rises(run(gate, false))) << "release block " << block;
  }
  EXPECT_TRUE(silent(run(gate, false)));
  EXPECT_EQ(gate.gain(), 0.0F);
  EXPECT_EQ(gate.closed_blocks(), static_cast<std::uint64_t>(kReleaseBlocks + 1));
  EXPECT_EQ(gate.blocks(), static_cast<std::uint64_t>(kHoldBlocks + kReleaseBlocks));
}

TEST(VoiceGate, AVoiceOpensItWithinTheBlockItArrivesIn) {
  VoiceGate gate;
  gate.reset(kRate);
  for (int block = 0; block < kHoldBlocks + kReleaseBlocks + 5; ++block) {
    run(gate, false);
  }
  ASSERT_TRUE(silent(run(gate, false)));

  // The attack is one block long: the first sample is nearly gone and the
  // last is all there, with nothing but a straight line between them.
  // Near rather than equal: 480 steps of a 480th add up to one only in exact
  // arithmetic, and the block after this one is where the gain is clamped
  // onto one and stays there.
  const std::vector<float> first = run(gate, true);
  EXPECT_TRUE(gate.open());
  EXPECT_LT(first.front(), 0.01F);
  EXPECT_NEAR(first.back(), 1.0F, 1e-4F);
  EXPECT_TRUE(never_falls(first));

  EXPECT_TRUE(untouched(run(gate, true)));
}

TEST(VoiceGate, APauseShorterThanTheHoldIsNotTheEndOfTheSentence) {
  VoiceGate gate;
  gate.reset(kRate);
  run(gate, true);
  for (int block = 0; block < kHoldBlocks - 5; ++block) {
    EXPECT_TRUE(untouched(run(gate, false))) << "pause block " << block;
  }
  EXPECT_TRUE(untouched(run(gate, true)));
  EXPECT_EQ(gate.closed_blocks(), 0U);
}

TEST(VoiceGate, AVoiceDuringTheReleaseTurnsItAroundWithoutAJump) {
  VoiceGate gate;
  gate.reset(kRate);
  for (int block = 0; block < kHoldBlocks + 3; ++block) {
    run(gate, false);
  }
  const float halfway = gate.gain();
  ASSERT_GT(halfway, 0.0F);
  ASSERT_LT(halfway, 1.0F);

  // Back up from wherever it was, one step at a time. A gate that snapped to
  // one here would click in the middle of a word.
  const std::vector<float> back = run(gate, true);
  EXPECT_GE(back.front(), halfway);
  EXPECT_TRUE(never_falls(back));
  EXPECT_NEAR(back.back(), 1.0F, 1e-4F);
}

TEST(VoiceGate, ResetForgetsTheRoom) {
  VoiceGate gate;
  gate.reset(kRate);
  for (int block = 0; block < kHoldBlocks + kReleaseBlocks + 5; ++block) {
    run(gate, false);
  }
  ASSERT_FALSE(gate.open());

  gate.reset(kRate);
  EXPECT_TRUE(gate.open());
  EXPECT_EQ(gate.gain(), 1.0F);
  EXPECT_EQ(gate.blocks(), 0U);
  EXPECT_EQ(gate.closed_blocks(), 0U);
  EXPECT_TRUE(untouched(run(gate, false)));
}

TEST(VoiceGate, TheTimingsAreMeasuredInSamplesOfTheRateGiven) {
  // 100 ms of hold at 16 kHz is 1600 samples, ten blocks of 160: the tenth
  // silent block is the one that closes, and the release, 50 ms, is five
  // blocks long.
  VoiceGate gate(VoiceGateTiming{.attack_ms = 10, .hold_ms = 100, .release_ms = 50});
  gate.reset(16000);
  constexpr std::size_t kSmallBlock = 160;
  const auto run_small = [&gate](bool voiced) {
    std::vector<float> block(kSmallBlock, 1.0F);
    float* channel = block.data();
    gate.apply(voiced, &channel, 1, kSmallBlock);
    return block;
  };

  for (int block = 0; block < 9; ++block) {
    EXPECT_TRUE(untouched(run_small(false))) << "block " << block;
  }
  EXPECT_FALSE(untouched(run_small(false)));
  EXPECT_FALSE(gate.open());
  for (int block = 0; block < 4; ++block) {
    EXPECT_FALSE(silent(run_small(false))) << "release block " << block;
  }
  EXPECT_TRUE(silent(run_small(false)));
}

TEST(VoiceGate, EveryChannelGetsTheSameGain) {
  VoiceGate gate;
  gate.reset(kRate);
  for (int block = 0; block < kHoldBlocks - 1; ++block) {
    run(gate, false);
  }

  std::vector<float> left(kBlock, 1.0F);
  std::vector<float> right(kBlock, 1.0F);
  float* channels[] = {left.data(), right.data()};
  gate.apply(false, channels, 2, kBlock);
  EXPECT_EQ(left, right);
  EXPECT_FALSE(untouched(left));
}

using dv::client::media::parse_voice_gate_level;
using dv::client::media::to_string;
using dv::client::media::VoiceGateLevel;

TEST(VoiceGateLevel, EveryLevelRoundTripsThroughItsSpelling) {
  for (const VoiceGateLevel level : {VoiceGateLevel::Low, VoiceGateLevel::Moderate,
                                     VoiceGateLevel::High, VoiceGateLevel::VeryHigh}) {
    EXPECT_EQ(parse_voice_gate_level(to_string(level)), level);
  }
}

TEST(VoiceGateLevel, TheSpellingIsTheConfigurationsOwn) {
  // The words docs/03-configuration.md documents and config.cpp accepts, the
  // same four the suppressor uses; changing one here without the other is a
  // level nobody can set.
  EXPECT_EQ(to_string(VoiceGateLevel::Low), "low");
  EXPECT_EQ(to_string(VoiceGateLevel::Moderate), "moderate");
  EXPECT_EQ(to_string(VoiceGateLevel::High), "high");
  EXPECT_EQ(to_string(VoiceGateLevel::VeryHigh), "very_high");
}

TEST(VoiceGateLevel, AWordThatIsNotALevelIsNothing) {
  EXPECT_EQ(parse_voice_gate_level("strict"), std::nullopt);
  EXPECT_EQ(parse_voice_gate_level("Moderate"), std::nullopt);
  EXPECT_EQ(parse_voice_gate_level(""), std::nullopt);
}

}  // namespace
