// The voice gate as the processing module sees it: a capture post-processor
// handed a real webrtc::AudioBuffer, one 10 ms block at a time.
//
// Two detectors. A scripted one, which says what the test tells it to, is
// what proves the processor: that it gates, that it stops gating when told
// to, that the level reaches the detector on the thread that uses it. The
// library's own detector is then heard on the two signals it cannot get wrong
// - digital silence and a loud buzz with the shape of a voice - which is what
// proves it is wired up and not merely present.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numbers>
#include <optional>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <modules/audio_processing/audio_buffer.h>

#include "webrtc/voice_gate_processor.hpp"

namespace {

using dv::client::media::VoiceDetector;
using dv::client::media::VoiceGateLevel;
using dv::client::media::VoiceGateProcessor;

constexpr int kRate = 48000;
constexpr std::size_t kBlock = 480;
constexpr int kHoldBlocks = 30;
constexpr int kReleaseBlocks = 10;
/// Loud enough for a detector to notice and well inside 16 bits.
constexpr float kLevel = 1000.0F;

class ScriptedDetector final : public VoiceDetector {
 public:
  bool initialize(int sample_rate_hz, std::size_t frames) override {
    sample_rate_hz_ = sample_rate_hz;
    frames_ = frames;
    level_.reset();
    return accepts_;
  }
  void set_level(VoiceGateLevel level) override { level_ = level; }
  bool voiced(const std::int16_t* /*samples*/, std::size_t /*frames*/) override {
    ++asked_;
    return answer_;
  }

  bool accepts_ = true;
  bool answer_ = false;
  std::optional<VoiceGateLevel> level_;
  int asked_ = 0;
  int sample_rate_hz_ = 0;
  std::size_t frames_ = 0;
};

/// One block of `sample` through the gate, and what came out.
std::vector<float> process(VoiceGateProcessor& gate, float sample) {
  webrtc::AudioBuffer buffer(kRate, 1, kRate, 1, kRate, 1);
  std::fill_n(buffer.channels()[0], buffer.num_frames(), sample);
  gate.Process(&buffer);
  return {buffer.channels()[0], buffer.channels()[0] + buffer.num_frames()};
}

bool all_at(const std::vector<float>& block, float value) {
  return std::all_of(block.begin(), block.end(), [value](float sample) { return sample == value; });
}

TEST(VoiceGateProcessor, LetsTheMicrophoneThroughForAHoldAndThenSilencesIt) {
  auto owned = std::make_unique<ScriptedDetector>();
  ScriptedDetector* detector = owned.get();
  VoiceGateProcessor gate(std::move(owned));
  gate.Initialize(kRate, 1);
  EXPECT_EQ(detector->sample_rate_hz_, kRate);
  EXPECT_EQ(detector->frames_, kBlock);

  for (int block = 0; block < kHoldBlocks - 1; ++block) {
    EXPECT_TRUE(all_at(process(gate, kLevel), kLevel)) << "block " << block;
    EXPECT_TRUE(gate.state().open);
  }
  for (int block = 0; block < kReleaseBlocks + 1; ++block) {
    process(gate, kLevel);
  }
  EXPECT_TRUE(all_at(process(gate, kLevel), 0.0F));

  const VoiceGateProcessor::State state = gate.state();
  EXPECT_TRUE(state.enabled);
  EXPECT_FALSE(state.open);
  EXPECT_EQ(state.blocks, static_cast<std::uint64_t>(kHoldBlocks + kReleaseBlocks + 1));
  EXPECT_EQ(state.closed_blocks, static_cast<std::uint64_t>(kReleaseBlocks + 2));
  EXPECT_EQ(detector->asked_, kHoldBlocks + kReleaseBlocks + 1);
}

TEST(VoiceGateProcessor, OpensOnTheFirstBlockWithAVoiceInIt) {
  auto owned = std::make_unique<ScriptedDetector>();
  ScriptedDetector* detector = owned.get();
  VoiceGateProcessor gate(std::move(owned));
  gate.Initialize(kRate, 1);
  for (int block = 0; block < kHoldBlocks + kReleaseBlocks + 2; ++block) {
    process(gate, kLevel);
  }
  ASSERT_FALSE(gate.state().open);

  detector->answer_ = true;
  const std::vector<float> first = process(gate, kLevel);
  EXPECT_TRUE(gate.state().open);
  EXPECT_LT(first.front(), kLevel / 100);
  EXPECT_NEAR(first.back(), kLevel, 0.1F);
  EXPECT_TRUE(all_at(process(gate, kLevel), kLevel));
}

TEST(VoiceGateProcessor, OffMeansUntouchedAndUnasked) {
  auto owned = std::make_unique<ScriptedDetector>();
  ScriptedDetector* detector = owned.get();
  VoiceGateProcessor gate(std::move(owned));
  gate.Initialize(kRate, 1);
  gate.configure(false, VoiceGateLevel::Moderate);

  for (int block = 0; block < kHoldBlocks + kReleaseBlocks + 5; ++block) {
    EXPECT_TRUE(all_at(process(gate, kLevel), kLevel)) << "block " << block;
  }
  EXPECT_EQ(detector->asked_, 0);
  EXPECT_FALSE(gate.state().enabled);
  EXPECT_TRUE(gate.state().open);
}

TEST(VoiceGateProcessor, SwitchingBackOnStartsWithAFullHold) {
  // Off in the middle of the release, then on again: the gate is open with a
  // whole hold ahead of it, not two blocks from silence.
  auto owned = std::make_unique<ScriptedDetector>();
  VoiceGateProcessor gate(std::move(owned));
  gate.Initialize(kRate, 1);
  for (int block = 0; block < kHoldBlocks + 3; ++block) {
    process(gate, kLevel);
  }
  ASSERT_FALSE(gate.state().open);

  gate.configure(false, VoiceGateLevel::Moderate);
  process(gate, kLevel);
  gate.configure(true, VoiceGateLevel::Moderate);
  for (int block = 0; block < kHoldBlocks - 1; ++block) {
    EXPECT_TRUE(all_at(process(gate, kLevel), kLevel)) << "block " << block;
  }
}

TEST(VoiceGateProcessor, TheLevelReachesTheDetectorOnTheProcessingThread) {
  auto owned = std::make_unique<ScriptedDetector>();
  ScriptedDetector* detector = owned.get();
  VoiceGateProcessor gate(std::move(owned));
  gate.Initialize(kRate, 1);

  gate.configure(true, VoiceGateLevel::VeryHigh);
  EXPECT_EQ(detector->level_, std::nullopt) << "configure is any thread; the detector is not";
  process(gate, kLevel);
  EXPECT_EQ(detector->level_, VoiceGateLevel::VeryHigh);

  // A new format resets the detector, and with it the mode: the level goes
  // in again with the next block.
  gate.Initialize(kRate, 1);
  EXPECT_EQ(detector->level_, std::nullopt);
  process(gate, kLevel);
  EXPECT_EQ(detector->level_, VoiceGateLevel::VeryHigh);
  EXPECT_EQ(gate.state().level, VoiceGateLevel::VeryHigh);
}

TEST(VoiceGateProcessor, AFormatTheDetectorCannotJudgeGoesThroughUngated) {
  auto owned = std::make_unique<ScriptedDetector>();
  ScriptedDetector* detector = owned.get();
  detector->accepts_ = false;
  VoiceGateProcessor gate(std::move(owned));
  gate.Initialize(kRate, 1);

  for (int block = 0; block < kHoldBlocks + kReleaseBlocks + 5; ++block) {
    EXPECT_TRUE(all_at(process(gate, kLevel), kLevel)) << "block " << block;
  }
  EXPECT_EQ(detector->asked_, 0);
  EXPECT_EQ(gate.state().blocks, 0U);
  EXPECT_TRUE(gate.state().open);
}

TEST(VoiceGateProcessor, LibwebrtcsDetectorHearsSilenceAsSilence) {
  VoiceGateProcessor gate;
  gate.Initialize(kRate, 1);
  for (int block = 0; block < kHoldBlocks + kReleaseBlocks + 5; ++block) {
    process(gate, 0.0F);
  }
  const VoiceGateProcessor::State state = gate.state();
  EXPECT_FALSE(state.open);
  EXPECT_GT(state.blocks, 0U);
  EXPECT_GT(state.closed_blocks, 0U);
}

TEST(VoiceGateProcessor, LibwebrtcsDetectorHearsABuzzWithTheShapeOfAVoice) {
  // A fundamental at 140 Hz with its first twenty-five harmonics, which puts
  // energy in every band the detector listens to, swelling four times a
  // second the way syllables do. Not a voice, but everything the detector
  // measures of one.
  VoiceGateProcessor gate;
  gate.Initialize(kRate, 1);
  gate.configure(true, VoiceGateLevel::Moderate);

  std::uint64_t open_blocks = 0;
  constexpr int kBlocks = 100;
  for (int block = 0; block < kBlocks; ++block) {
    webrtc::AudioBuffer buffer(kRate, 1, kRate, 1, kRate, 1);
    float* samples = buffer.channels()[0];
    for (std::size_t i = 0; i < buffer.num_frames(); ++i) {
      const double t = (static_cast<double>(block) * kBlock + static_cast<double>(i)) / kRate;
      double value = 0;
      for (int harmonic = 1; harmonic <= 25; ++harmonic) {
        value += std::sin(2 * std::numbers::pi * 140.0 * harmonic * t) / harmonic;
      }
      const double syllable = 0.6 + (0.4 * std::sin(2 * std::numbers::pi * 4.0 * t));
      samples[i] = static_cast<float>(value * syllable * 6000.0);
    }
    gate.Process(&buffer);
    if (gate.state().open) {
      ++open_blocks;
    }
  }
  // The first hold is open by design, so the count is over the blocks after
  // it: the detector has to be keeping the gate open on its own by then.
  EXPECT_TRUE(gate.state().open);
  EXPECT_GT(open_blocks, static_cast<std::uint64_t>(kBlocks - 10));
}

}  // namespace
