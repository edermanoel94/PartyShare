// The arithmetic of mixing the shared screen's sound into the microphone.
//
// audio::ScreenAudioMixer holds no libwebrtc type on purpose, so everything
// that could be got wrong here - gain, saturation, channel counts, what happens
// while the capture is stopped - is testable without a call, a sound card or
// the toolchain of docs/webrtc-toolchain.md.
//
// What is *not* tested here is the mixing with real screen audio in it: that
// needs a capture running, which is tests/integration/test_loopback_capture.cpp
// and, end to end, the spike in tools/screen_audio_spike.

#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "app/call_session.hpp"
#include "audio/block_pacer.hpp"
#include "audio/screen_audio_mixer.hpp"

namespace {

using dv::client::app::screen_audio_mode_from;
using dv::client::app::ScreenAudio;

TEST(ScreenAudioModeTest, TheNamesRoundTrip) {
  // Configuration, the settings dialog and the capture all have to mean the
  // same thing by these words, which is why the mapping lives in one place.
  for (const ScreenAudio::Mode mode :
       {ScreenAudio::Mode::None, ScreenAudio::Mode::System, ScreenAudio::Mode::Application}) {
    EXPECT_EQ(screen_audio_mode_from(dv::client::app::to_string(mode)), mode);
  }
}

TEST(ScreenAudioModeTest, AnythingUnrecognisedIsSilence) {
  // A configuration written by a later version, or a typo. Falling back to
  // capturing the machine because a word was not understood is the one answer
  // that must never happen.
  EXPECT_EQ(screen_audio_mode_from("everything"), ScreenAudio::Mode::None);
  EXPECT_EQ(screen_audio_mode_from(""), ScreenAudio::Mode::None);
  EXPECT_EQ(screen_audio_mode_from("System"), ScreenAudio::Mode::None);
}

using dv::client::audio::kFramesPerBlock;
using dv::client::audio::kSamplesPerBlock;
using dv::client::audio::LoopbackMode;
using dv::client::audio::MixResult;
using dv::client::audio::ScreenAudioMixer;

/// One block of a constant value, mono, which is what the audio processing
/// module hands over.
[[nodiscard]] std::vector<std::int16_t> microphone(std::int16_t value,
                                                   std::size_t frames = kFramesPerBlock) {
  return std::vector<std::int16_t>(frames, value);
}

TEST(ScreenAudioMixerTest, WithNoShareTheMicrophonePassesThroughUntouched) {
  ScreenAudioMixer mixer;
  const std::vector<std::int16_t> input = microphone(1000);
  std::vector<std::int16_t> out(kSamplesPerBlock, -1);

  const MixResult result = mixer.mix(input, 1, out);

  // Still mono: going stereo for a call with no screen audio in it would ask
  // the encoder to carry two identical channels for nothing.
  EXPECT_EQ(result.channels, 1U);
  EXPECT_FALSE(result.screen_audio);
  for (std::size_t i = 0; i < kFramesPerBlock; ++i) {
    ASSERT_EQ(out[i], 1000) << "at frame " << i;
  }
}

TEST(ScreenAudioMixerTest, TheLevelIsTheMicrophoneBeforeTheGain) {
  ScreenAudioMixer mixer;
  std::vector<std::int16_t> out(kSamplesPerBlock, 0);

  // Half of full scale, constant, so the root mean square is exactly that.
  const MixResult loud = mixer.mix(microphone(16384), 1, out);
  EXPECT_NEAR(loud.microphone_level, 0.5, 0.001);
  EXPECT_NEAR(mixer.microphone_level(), 0.5, 0.001);

  const MixResult quiet = mixer.mix(microphone(0), 1, out);
  EXPECT_NEAR(quiet.microphone_level, 0.0, 0.001);
}

TEST(ScreenAudioMixerTest, MutingSilencesTheMicrophoneWithoutSilencingTheTrack) {
  // This is what replaces track->set_enabled(false) while a share is on:
  // disabling the track would take the film with it.
  ScreenAudioMixer mixer;
  std::vector<std::int16_t> out(kSamplesPerBlock, -1);

  mixer.set_microphone_muted(true);
  EXPECT_TRUE(mixer.microphone_muted());

  const MixResult result = mixer.mix(microphone(20000), 1, out);

  for (std::size_t i = 0; i < kFramesPerBlock; ++i) {
    ASSERT_EQ(out[i], 0) << "at frame " << i;
  }
  // The level still reports what the microphone actually heard. It is measured
  // before the gain on purpose: an indicator that reads zero because of the
  // mute tells the user nothing they did not already know, and this way the
  // "you are talking while muted" hint of a later phase has something to read.
  EXPECT_NEAR(result.microphone_level, 20000.0 / 32768.0, 0.001);
}

TEST(ScreenAudioMixerTest, UnmutingBringsItBack) {
  ScreenAudioMixer mixer;
  std::vector<std::int16_t> out(kSamplesPerBlock, 0);

  mixer.set_microphone_muted(true);
  mixer.mix(microphone(500), 1, out);
  EXPECT_EQ(out[0], 0);

  mixer.set_microphone_muted(false);
  mixer.mix(microphone(500), 1, out);
  EXPECT_EQ(out[0], 500);
}

TEST(ScreenAudioMixerTest, AFrameThatIsNotOneBlockPassesThrough) {
  // The audio processing module is configured for 48 kHz and hands over 10 ms
  // at a time. Anything else means an assumption broke somewhere above, and
  // mixing on a guess would be worse than not mixing.
  ScreenAudioMixer mixer;
  const std::vector<std::int16_t> input = microphone(700, 160);
  std::vector<std::int16_t> out(kSamplesPerBlock, -1);

  const MixResult result = mixer.mix(input, 1, out);

  EXPECT_EQ(result.channels, 1U);
  EXPECT_FALSE(result.screen_audio);
  EXPECT_EQ(out[0], 700);
  EXPECT_EQ(out[159], 700);
}

TEST(ScreenAudioMixerTest, AnEmptyFrameIsNotAnExcuseToReadIt) {
  ScreenAudioMixer mixer;
  std::vector<std::int16_t> out(kSamplesPerBlock, 0);

  const MixResult empty = mixer.mix({}, 1, out);
  EXPECT_EQ(empty.channels, 1U);
  EXPECT_EQ(empty.microphone_level, 0.0);

  const MixResult no_channels = mixer.mix(microphone(100), 0, out);
  EXPECT_EQ(no_channels.microphone_level, 0.0);
}

/// Two blocks of a constant per channel, which is what a capture delivers.
[[nodiscard]] std::vector<std::int16_t> screen(std::int16_t left, std::int16_t right,
                                               std::size_t blocks = 3) {
  std::vector<std::int16_t> samples;
  samples.reserve(blocks * kSamplesPerBlock);
  for (std::size_t i = 0; i < blocks * kFramesPerBlock; ++i) {
    samples.push_back(left);
    samples.push_back(right);
  }
  return samples;
}

TEST(ScreenAudioMixerTest, ScreenAudioArrivesInStereoAndTheVoiceIsInBothEars) {
  ScreenAudioMixer mixer;
  mixer.push_screen_audio(screen(1000, -2000));

  std::vector<std::int16_t> out(kSamplesPerBlock, 0);
  const MixResult result = mixer.mix(microphone(300), 1, out);

  ASSERT_EQ(result.channels, 2U);
  EXPECT_TRUE(result.screen_audio);
  for (std::size_t frame = 0; frame < kFramesPerBlock; ++frame) {
    // A mono microphone goes to both sides; the screen keeps its own.
    ASSERT_EQ(out[frame * 2], 300 + 1000) << "left, at frame " << frame;
    ASSERT_EQ(out[(frame * 2) + 1], 300 - 2000) << "right, at frame " << frame;
  }
}

TEST(ScreenAudioMixerTest, ALoudFilmAndALoudVoiceClipRatherThanWrapAround) {
  // Two signals near full scale add to more than an int16 holds. Wrapping
  // around would turn the loudest moment of a film into a burst of noise, which
  // is the one failure a listener cannot mistake for anything else.
  ScreenAudioMixer mixer;
  mixer.push_screen_audio(screen(30000, -30000));

  std::vector<std::int16_t> out(kSamplesPerBlock, 0);
  mixer.mix(microphone(20000), 1, out);

  EXPECT_EQ(out[0], 32767);
  EXPECT_EQ(out[1], -10000);
}

TEST(ScreenAudioMixerTest, ItClipsAtTheBottomToo) {
  // A mixer of its own rather than a second push into the one above: what is
  // already buffered comes out first, so pushing something new does not change
  // what the next block holds.
  ScreenAudioMixer mixer;
  mixer.push_screen_audio(screen(-30000, -30000));

  std::vector<std::int16_t> out(kSamplesPerBlock, 0);
  mixer.mix(microphone(-20000), 1, out);

  EXPECT_EQ(out[0], -32768);
  EXPECT_EQ(out[1], -32768);
}

TEST(ScreenAudioMixerTest, MutedWithAShareOnSendsTheFilmAndNotTheVoice) {
  // The whole reason muting stopped being track->set_enabled(false).
  ScreenAudioMixer mixer;
  mixer.set_microphone_muted(true);
  mixer.push_screen_audio(screen(4000, 5000));

  std::vector<std::int16_t> out(kSamplesPerBlock, 0);
  const MixResult result = mixer.mix(microphone(9999), 1, out);

  ASSERT_EQ(result.channels, 2U);
  EXPECT_EQ(out[0], 4000);
  EXPECT_EQ(out[1], 5000);
}

TEST(ScreenAudioMixerTest, AStarvedCaptureCostsSilenceAndNotTheVoice) {
  ScreenAudioMixer mixer;
  // One block only, which is under the pacer's prime, so nothing comes out of
  // it yet.
  mixer.push_screen_audio(screen(6000, 6000, 1));

  std::vector<std::int16_t> out(kSamplesPerBlock, 0);
  const MixResult result = mixer.mix(microphone(250), 1, out);

  // Still stereo, still carrying the microphone: a starving capture must never
  // take the voice with it.
  EXPECT_EQ(result.channels, 2U);
  EXPECT_FALSE(result.screen_audio);
  EXPECT_EQ(out[0], 250);
  EXPECT_EQ(out[1], 250);
}

TEST(ScreenAudioMixerTest, StoppingForgetsWhatWasBuffered) {
  ScreenAudioMixer mixer;
  mixer.push_screen_audio(screen(7000, 7000));
  mixer.stop();

  std::vector<std::int16_t> out(kSamplesPerBlock, 0);
  const MixResult result = mixer.mix(microphone(100), 1, out);

  // Back to a plain mono microphone, with none of the old film left in it.
  EXPECT_EQ(result.channels, 1U);
  EXPECT_EQ(out[0], 100);
}

TEST(ScreenAudioMixerTest, NothingIsActiveUntilACaptureStarts) {
  ScreenAudioMixer mixer;
  EXPECT_FALSE(mixer.active());
  EXPECT_TRUE(mixer.failure().code.empty());

  // Zero is not a process, on every platform. On Windows this is the mixer
  // refusing; everywhere else the capturer does not exist at all. Either way
  // nothing starts and nothing is left half open.
  const auto started = mixer.start(LoopbackMode::Process, 0);
  EXPECT_FALSE(started.ok());
  EXPECT_FALSE(mixer.active());
}

TEST(ScreenAudioMixerTest, StoppingWhatNeverStartedIsHarmless) {
  ScreenAudioMixer mixer;
  mixer.stop();
  mixer.stop();
  EXPECT_FALSE(mixer.active());

  std::vector<std::int16_t> out(kSamplesPerBlock, 0);
  const MixResult result = mixer.mix(microphone(1), 1, out);
  EXPECT_EQ(result.channels, 1U);
}

// --- the volume of the shared screen --------------------------------------

using dv::client::audio::kMaxScreenVolumePercent;
using dv::client::audio::screen_volume_ratio;

TEST(ScreenVolumeRatioTest, ThePercentageIsTheRatio) {
  // Linear, which is the choice this control makes on purpose: it balances a
  // screen against a microphone, so the number on screen and the arithmetic in
  // the mix say the same thing. See screen_volume_ratio.
  EXPECT_DOUBLE_EQ(screen_volume_ratio(0), 0.0);
  EXPECT_DOUBLE_EQ(screen_volume_ratio(50), 0.5);
  EXPECT_DOUBLE_EQ(screen_volume_ratio(100), 1.0);
  EXPECT_DOUBLE_EQ(screen_volume_ratio(200), 2.0);
}

TEST(ScreenVolumeRatioTest, AnythingOutOfRangeIsClampedRatherThanRefused) {
  // Reached from a saved configuration as well as from a slider, and a file
  // holding 500 should be loud rather than fatal.
  EXPECT_DOUBLE_EQ(screen_volume_ratio(-40), 0.0);
  EXPECT_DOUBLE_EQ(screen_volume_ratio(10000), 2.0);
  EXPECT_DOUBLE_EQ(screen_volume_ratio(kMaxScreenVolumePercent + 1), 2.0);
}

TEST(ScreenAudioMixerTest, TheVolumeStartsWhereAShareUsedToBe) {
  // A guard on the setting existing at all rather than on its default: every
  // share before this went out untouched, and a configuration written before
  // the key existed has to keep meaning that.
  ScreenAudioMixer mixer;
  EXPECT_EQ(mixer.screen_volume(), 100);

  mixer.push_screen_audio(screen(1000, -2000));
  std::vector<std::int16_t> out(kSamplesPerBlock, 0);
  mixer.mix(microphone(300), 1, out);

  EXPECT_EQ(out[0], 300 + 1000);
  EXPECT_EQ(out[1], 300 - 2000);
}

TEST(ScreenAudioMixerTest, HalfVolumeHalvesTheScreenAndLeavesTheVoiceAlone) {
  ScreenAudioMixer mixer;
  mixer.set_screen_volume(50);
  EXPECT_EQ(mixer.screen_volume(), 50);

  mixer.push_screen_audio(screen(1000, -2000));
  std::vector<std::int16_t> out(kSamplesPerBlock, 0);
  const MixResult result = mixer.mix(microphone(300), 1, out);

  ASSERT_EQ(result.channels, 2U);
  // The microphone keeps its 300 in both ears. Quietening the voice as well
  // would make this a master volume, and the voice already has a gain control -
  // libwebrtc's, running before any of this.
  EXPECT_EQ(out[0], 300 + 500);
  EXPECT_EQ(out[1], 300 - 1000);
}

TEST(ScreenAudioMixerTest, ANegativeSampleIsQuietenedAndNotInverted) {
  // The fixed point shift is arithmetic, which is what a signed right shift
  // means from C++20 on. Filled with zeros instead, the negative half of every
  // waveform would come back as a large positive number: the sound of the
  // volume being turned down would be the film turning into noise.
  ScreenAudioMixer mixer;
  mixer.set_screen_volume(25);
  mixer.push_screen_audio(screen(-8000, -1));

  std::vector<std::int16_t> out(kSamplesPerBlock, 0);
  mixer.mix(microphone(0), 1, out);

  EXPECT_EQ(out[0], -2000);
  // Still negative after rounding, and nowhere near +32767.
  EXPECT_LE(out[1], 0);
  EXPECT_GE(out[1], -1);
}

TEST(ScreenAudioMixerTest, ZeroVolumeIsSilenceFromTheScreenAndNotFromTheTrack) {
  // The counterpart of muting the microphone: one source goes quiet and the
  // other carries on, rather than the track going away.
  ScreenAudioMixer mixer;
  mixer.set_screen_volume(0);
  mixer.push_screen_audio(screen(9000, 9000));

  std::vector<std::int16_t> out(kSamplesPerBlock, 0);
  const MixResult result = mixer.mix(microphone(400), 1, out);

  // Still stereo, and still reporting that a block was taken: the capture is
  // running and being consumed, it is simply being scaled to nothing.
  ASSERT_EQ(result.channels, 2U);
  EXPECT_TRUE(result.screen_audio);
  EXPECT_EQ(out[0], 400);
  EXPECT_EQ(out[1], 400);
}

TEST(ScreenAudioMixerTest, ABoostClipsRatherThanWrappingAround) {
  // The reason above 100% is offered at all: saturate() is already the net
  // under it, so the worst a boost can do is sound harsh. Wrapping would turn
  // the loudest moment into a burst of noise instead.
  ScreenAudioMixer mixer;
  mixer.set_screen_volume(kMaxScreenVolumePercent);
  mixer.push_screen_audio(screen(20000, -20000));

  std::vector<std::int16_t> out(kSamplesPerBlock, 0);
  mixer.mix(microphone(0), 1, out);

  EXPECT_EQ(out[0], 32767);
  EXPECT_EQ(out[1], -32768);
}

TEST(ScreenAudioMixerTest, AQuietSourceIsWhatTheBoostIsFor) {
  // The case the ceiling exists for: an application playing at a tenth of its
  // scale, brought back up with headroom to spare and nothing clipped.
  ScreenAudioMixer mixer;
  mixer.set_screen_volume(200);
  mixer.push_screen_audio(screen(3000, -3000));

  std::vector<std::int16_t> out(kSamplesPerBlock, 0);
  mixer.mix(microphone(0), 1, out);

  EXPECT_EQ(out[0], 6000);
  EXPECT_EQ(out[1], -6000);
}

TEST(ScreenAudioMixerTest, TheVolumeIsRememberedAcrossAStop) {
  // The mixer outlives every call in the process, so a level chosen during one
  // share is the level the next one starts at. A stop that reset it would make
  // this setting last exactly as long as one share.
  ScreenAudioMixer mixer;
  mixer.set_screen_volume(30);
  mixer.stop();
  EXPECT_EQ(mixer.screen_volume(), 30);

  mixer.push_screen_audio(screen(1000, 1000));
  std::vector<std::int16_t> out(kSamplesPerBlock, 0);
  mixer.mix(microphone(0), 1, out);
  EXPECT_EQ(out[0], 300);
}

TEST(ScreenAudioMixerTest, TheMixerClampsWhatTheInterfaceShouldHaveClamped) {
  // Belt and braces, and not a duplicate of the clamp in CallSession: that one
  // keeps the remembered value honest, this one keeps the arithmetic safe for
  // whoever reaches the mixer without going through it.
  ScreenAudioMixer mixer;
  mixer.set_screen_volume(-10);
  EXPECT_EQ(mixer.screen_volume(), 0);

  mixer.set_screen_volume(9999);
  EXPECT_EQ(mixer.screen_volume(), kMaxScreenVolumePercent);
}

TEST(ScreenAudioMixerTest, TheVolumeDoesNotTouchAShareThatCarriesNoSound) {
  // With no capture and nothing buffered the microphone goes straight through,
  // mono, whatever the volume says. The screen gain must not reach it.
  ScreenAudioMixer mixer;
  mixer.set_screen_volume(0);

  std::vector<std::int16_t> out(kSamplesPerBlock, -1);
  const MixResult result = mixer.mix(microphone(1234), 1, out);

  EXPECT_EQ(result.channels, 1U);
  EXPECT_FALSE(result.screen_audio);
  EXPECT_EQ(out[0], 1234);
}

}  // namespace
