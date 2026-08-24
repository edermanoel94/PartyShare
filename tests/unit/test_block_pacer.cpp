// The part of the screen audio capture that has no platform in it.
//
// BlockPacer is where the two clocks meet - the one the operating system
// delivers audio on and the one that has to hand out a block every 10 ms - so
// it is where drift, starving and overflow turn into decisions. All of that is
// pure logic, and none of it needs a sound card.

#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "audio/block_pacer.hpp"

namespace {

using dv::client::audio::BlockPacer;
using dv::client::audio::kChannels;
using dv::client::audio::kFramesPerBlock;
using dv::client::audio::kSampleRateHz;
using dv::client::audio::kSamplesPerBlock;

/// `frames` frames of a constant value, so that a block can be identified by
/// looking at any sample in it.
[[nodiscard]] std::vector<std::int16_t> tone(std::size_t frames, std::int16_t value) {
  return std::vector<std::int16_t>(frames * kChannels, value);
}

[[nodiscard]] bool all_silent(const std::vector<std::int16_t>& block) {
  for (const std::int16_t sample : block) {
    if (sample != 0) {
      return false;
    }
  }
  return true;
}

TEST(BlockPacerTest, TheFormatIsWhatTheAudioPathSpeaks) {
  EXPECT_EQ(kSampleRateHz, 48000);
  EXPECT_EQ(kChannels, 2U);
  EXPECT_EQ(kFramesPerBlock, 480U);
  EXPECT_EQ(kSamplesPerBlock, 960U);
}

TEST(BlockPacerTest, AnEmptyPacerHandsOutSilenceRatherThanNothing) {
  BlockPacer pacer;
  std::vector<std::int16_t> block(kSamplesPerBlock, 1234);

  EXPECT_FALSE(pacer.take(block));
  EXPECT_TRUE(all_silent(block));
  EXPECT_EQ(pacer.stats().blocks_silent, 1U);
  EXPECT_EQ(pacer.stats().blocks_taken, 1U);
}

TEST(BlockPacerTest, AudioComesBackOutInOrder) {
  BlockPacer pacer{{.prime_frames = kFramesPerBlock}};
  for (std::int16_t value = 1; value <= 3; ++value) {
    pacer.push(tone(kFramesPerBlock, value));
  }

  for (std::int16_t value = 1; value <= 3; ++value) {
    std::vector<std::int16_t> block(kSamplesPerBlock, 0);
    ASSERT_TRUE(pacer.take(block));
    EXPECT_EQ(block.front(), value);
    EXPECT_EQ(block.back(), value);
  }
  EXPECT_EQ(pacer.buffered_frames(), 0U);
}

TEST(BlockPacerTest, NothingGoesOutUntilThePrimeIsReached) {
  // Half a block in a pacer that wants two blocks before it starts.
  BlockPacer pacer{{.prime_frames = kFramesPerBlock * 2}};
  pacer.push(tone(kFramesPerBlock / 2, 7));

  std::vector<std::int16_t> block(kSamplesPerBlock, 0);
  EXPECT_FALSE(pacer.take(block));
  EXPECT_TRUE(all_silent(block));
  // The audio was not thrown away, only held.
  EXPECT_EQ(pacer.buffered_frames(), kFramesPerBlock / 2);

  pacer.push(tone(kFramesPerBlock * 2, 7));
  EXPECT_TRUE(pacer.take(block));
  EXPECT_EQ(block.front(), 7);
}

TEST(BlockPacerTest, AStarveWaitsForARefillInsteadOfStuttering) {
  // The failure this prevents: a capture running a hair behind delivers audio,
  // silence, audio, silence at 10 ms each, which is worse to listen to than one
  // gap followed by continuous sound.
  BlockPacer pacer{{.prime_frames = kFramesPerBlock * 2}};
  pacer.push(tone(kFramesPerBlock * 2, 5));

  std::vector<std::int16_t> block(kSamplesPerBlock, 0);
  EXPECT_TRUE(pacer.take(block));
  EXPECT_TRUE(pacer.take(block));
  // Empty now, so this one is silence and the pacer goes back to waiting.
  EXPECT_FALSE(pacer.take(block));

  // One block arrives. It is not enough to reach the prime, so it stays put.
  pacer.push(tone(kFramesPerBlock, 6));
  EXPECT_FALSE(pacer.take(block));
  EXPECT_EQ(pacer.buffered_frames(), kFramesPerBlock);

  // The second one reaches it, and the audio starts again from the beginning of
  // what was held.
  pacer.push(tone(kFramesPerBlock, 6));
  EXPECT_TRUE(pacer.take(block));
  EXPECT_EQ(block.front(), 6);
}

TEST(BlockPacerTest, TooMuchBufferedIsLatencyAndIsThrownAway) {
  BlockPacer pacer{{.high_watermark_frames = kFramesPerBlock * 4,
                    .target_frames = kFramesPerBlock * 2,
                    .prime_frames = kFramesPerBlock}};

  // Six blocks of the old thing, then one of the new.
  pacer.push(tone(kFramesPerBlock * 6, 11));
  EXPECT_EQ(pacer.buffered_frames(), kFramesPerBlock * 2);
  EXPECT_EQ(pacer.stats().frames_dropped, kFramesPerBlock * 4);

  pacer.push(tone(kFramesPerBlock, 22));

  // What survived is the newest, not the oldest: the listener hears the present
  // late by a little, never the past on time.
  std::vector<std::int16_t> block(kSamplesPerBlock, 0);
  ASSERT_TRUE(pacer.take(block));
  EXPECT_EQ(block.front(), 11);
  ASSERT_TRUE(pacer.take(block));
  EXPECT_EQ(block.front(), 11);
  ASSERT_TRUE(pacer.take(block));
  EXPECT_EQ(block.front(), 22);
}

TEST(BlockPacerTest, SilenceIsPushedWithoutABufferOfZeros) {
  // Windows flags a silent packet rather than handing over zeros, and the pacer
  // has to be able to take it that way.
  BlockPacer pacer{{.prime_frames = kFramesPerBlock}};
  pacer.push_silence(kFramesPerBlock);

  std::vector<std::int16_t> block(kSamplesPerBlock, 99);
  EXPECT_TRUE(pacer.take(block));
  EXPECT_TRUE(all_silent(block));
  // It counts as captured: the application was playing, and what it played was
  // nothing.
  EXPECT_EQ(pacer.stats().frames_pushed, kFramesPerBlock);
  EXPECT_EQ(pacer.stats().blocks_silent, 0U);
}

TEST(BlockPacerTest, APacketThatIsNotAWholeNumberOfBlocksStillLinesUp) {
  // WASAPI accumulates whatever it likes: 480 frames may arrive as 80 plus 400,
  // or as 48 packets of 10.
  BlockPacer pacer{{.prime_frames = kFramesPerBlock}};
  pacer.push(tone(80, 3));
  pacer.push(tone(400, 3));
  pacer.push(tone(133, 4));

  std::vector<std::int16_t> block(kSamplesPerBlock, 0);
  ASSERT_TRUE(pacer.take(block));
  EXPECT_EQ(block.front(), 3);
  EXPECT_EQ(block.back(), 3);
  EXPECT_EQ(pacer.buffered_frames(), 133U);
}

TEST(BlockPacerTest, ClearForgetsEverythingIncludingThePrime) {
  BlockPacer pacer{{.prime_frames = kFramesPerBlock}};
  pacer.push(tone(kFramesPerBlock * 2, 8));
  pacer.clear();

  EXPECT_EQ(pacer.buffered_frames(), 0U);
  std::vector<std::int16_t> block(kSamplesPerBlock, 0);
  EXPECT_FALSE(pacer.take(block));
}

}  // namespace
