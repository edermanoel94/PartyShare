// The screen audio capture against the real operating system.
//
// Integration rather than unit because it activates a WASAPI loopback stream on
// the machine running the test, and renders a quiet 440 Hz tone for it to hear.
// Nothing is written to disk and nothing outside this process is touched.
//
// Two properties are worth the machinery. That a process loopback hears what
// the target process plays, which is the feature. And that the system-wide mode
// does not hear this process, which is what stops a share from sending every
// participant their own voice back - see
// docs/audio-da-tela-compartilhada.md, section 6.
//
// The third is quieter: blocks have to keep arriving on the clock while nothing
// plays at all. A process loopback produces no packets until there is sound, so
// a capture that only forwarded what Windows handed it would deliver nothing
// for minutes and then a burst. See audio::BlockPacer.

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <future>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <windows.h>
#endif

#include "audio/audio_sources.hpp"
#include "audio/block_pacer.hpp"
#include "audio/loopback_capturer.hpp"
#include "tone_player.hpp"

namespace {

using dv::client::audio::audio_sources;
using dv::client::audio::create_loopback_capturer;
using dv::client::audio::kSamplesPerBlock;
using dv::client::audio::loopback_capture_is_available;
using dv::client::audio::LoopbackMode;

TEST(LoopbackCaptureTest, AProcessLoopbackNeedsAProcess) {
  auto capturer = create_loopback_capturer([](std::span<const std::int16_t>) {}, [](dv::Error) {});
  if (!capturer) {
    GTEST_SKIP() << "no loopback capture here: " << capturer.error().message;
  }
  const auto started = std::move(capturer).take()->start(LoopbackMode::Process, 0);
  ASSERT_FALSE(started.ok());
  EXPECT_EQ(started.error().code, "invalid_value");
}

#if defined(_WIN32)

/// How much of TonePlayer's 440 Hz is in what was captured.
///
/// A loudness meter would not do. The machine running the test may well have a
/// browser playing something, and "did any sound arrive" cannot tell that apart
/// from "our tone arrived". Correlating against one frequency can: whatever
/// else is playing is not a pure 440 Hz sine.
///
/// Measured in windows of 100 ms rather than over the whole capture, because
/// the phase relationship between the render clock and the capture clock drifts
/// slowly, and a long correlation would smear the peak away.
class ToneDetector {
 public:
  static constexpr std::size_t kWindowFrames = 4800;

  /// Called only from the capture thread, and read only after it has been
  /// joined, so there is nothing here to synchronise.
  void add(std::span<const std::int16_t> block) {
    for (std::size_t i = 0; i + 1 < block.size(); i += 2) {
      window_.push_back((static_cast<double>(block[i]) + static_cast<double>(block[i + 1])) / 2.0 /
                        32768.0);
      if (window_.size() == kWindowFrames) {
        close();
      }
    }
  }

  /// The strongest 440 Hz any window held, roughly as an amplitude from 0 to 1.
  [[nodiscard]] double tone() const { return tone_; }

 private:
  void close() {
    double in_phase = 0;
    double quadrature = 0;
    const double step =
        2.0 * 3.14159265358979323846 * dv::testing::kTestToneHz / dv::client::audio::kSampleRateHz;
    for (std::size_t i = 0; i < window_.size(); ++i) {
      const double angle = step * static_cast<double>(i);
      in_phase += window_[i] * std::cos(angle);
      quadrature += window_[i] * std::sin(angle);
    }
    const auto count = static_cast<double>(window_.size());
    const double magnitude =
        2.0 * std::sqrt((in_phase * in_phase) + (quadrature * quadrature)) / count;
    tone_ = std::max(tone_, magnitude);
    window_.clear();
  }

  std::vector<double> window_;
  double tone_ = 0;
};

TEST(LoopbackCaptureTest, ItHearsWhatAProcessIsPlaying) {
  dv::testing::TonePlayer player;
  if (!player.start()) {
    GTEST_SKIP() << "this machine has no playback device to render a tone to";
  }

  ToneDetector detector;
  auto created = create_loopback_capturer(
      [&](std::span<const std::int16_t> block) { detector.add(block); }, [](dv::Error) {});
  ASSERT_TRUE(created.ok()) << created.error().message;
  auto capturer = std::move(created).take();

  // Our own process tree, which is where the tone is coming from. This is the
  // "share the Chrome tab" case with the test standing in for Chrome.
  const auto started =
      capturer->start(LoopbackMode::Process, static_cast<std::uint32_t>(GetCurrentProcessId()));
  if (!started.ok()) {
    GTEST_SKIP() << "the loopback would not start: " << started.error().message;
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(1500));
  capturer->stop();
  player.stop();

  // The tone leaves at 0.05 and the endpoint's own volume takes some of that,
  // so the bar sits an order of magnitude below what is sent and an order of
  // magnitude above what leaks through when nothing is playing.
  EXPECT_GT(detector.tone(), 0.005) << "the capture never heard the tone";
}

/// Runs a system-mode capture for a moment and returns how much 440 Hz was in
/// it. Used twice: once with this process silent and once with it playing.
[[nodiscard]] double system_mode_tone() {
  ToneDetector detector;
  auto created = create_loopback_capturer(
      [&](std::span<const std::int16_t> block) { detector.add(block); }, [](dv::Error) {});
  if (!created.ok()) {
    return -1;
  }
  auto capturer = std::move(created).take();
  if (!capturer->start(LoopbackMode::System, 0).ok()) {
    return -1;
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));
  capturer->stop();
  return detector.tone();
}

TEST(LoopbackCaptureTest, SystemModeIsDeafToThisProcess) {
  // The property the whole feature rests on. If system mode could hear us, a
  // share would send every other participant their own voice back, after the
  // echo canceller and with nothing left to remove it.
  //
  // Measured as a difference rather than against a fixed number. Asking "is it
  // silent" fails on a machine that has music playing, and asking "is there
  // 440 Hz" fails when the music happens to contain some - which it did, here,
  // at 0.0024 against a threshold of 0.001. What the property actually says is
  // that *our* tone changes nothing, so the baseline is measured first.
  const double baseline = system_mode_tone();
  if (baseline < 0) {
    GTEST_SKIP() << "no system loopback on this machine";
  }

  dv::testing::TonePlayer player;
  if (!player.start()) {
    GTEST_SKIP() << "this machine has no playback device to render a tone to";
  }
  const double with_tone = system_mode_tone();
  player.stop();

  ASSERT_GE(with_tone, 0.0);
  // Room for the machine's own audio to wander, and nowhere near the amount our
  // tone would add if it were being heard: captured directly, in the test
  // above, it lands two orders of magnitude higher than this.
  EXPECT_LT(with_tone, baseline + 0.005) << "system mode heard this process: " << baseline
                                         << " without the tone, " << with_tone << " with it";
}

TEST(LoopbackCaptureTest, BlocksArriveOnTheClockEvenWhenNothingIsPlaying) {
  // Windows 10 build 20348 or newer. Anything older has no per-process loopback
  // and the whole feature is off there.
  ASSERT_TRUE(loopback_capture_is_available());

  std::atomic<std::uint64_t> blocks{0};
  std::atomic<std::uint64_t> wrong_size{0};
  std::atomic<bool> failed{false};
  std::string failure;

  auto created = create_loopback_capturer(
      [&](std::span<const std::int16_t> block) {
        if (block.size() != kSamplesPerBlock) {
          ++wrong_size;
        }
        ++blocks;
      },
      [&](dv::Error error) {
        failure = error.message;
        failed = true;
      });
  ASSERT_TRUE(created.ok()) << created.error().message;
  auto capturer = std::move(created).take();

  const auto started = capturer->start(LoopbackMode::System, 0);
  if (!started.ok()) {
    // A machine with no audio stack at all, which is what a bare CI runner is.
    GTEST_SKIP() << "the loopback would not start: " << started.error().message;
  }
  EXPECT_TRUE(capturer->capturing());

  std::this_thread::sleep_for(std::chrono::seconds(1));
  const std::uint64_t after_a_second = blocks.load();
  capturer->stop();

  EXPECT_FALSE(capturer->capturing());
  EXPECT_FALSE(failed.load()) << failure;
  EXPECT_EQ(wrong_size.load(), 0U) << "a block was not exactly 10 ms";

  // A hundred blocks a second, give or take what the scheduler takes. The lower
  // bound is what matters: it is the difference between "silence is delivered"
  // and "nothing is delivered", which is the whole reason the pacer exists.
  EXPECT_GE(after_a_second, 80U);
  EXPECT_LE(after_a_second, 120U);

  // Whether those blocks carried audio or silence depends on what the machine
  // happened to be playing, and is not this test's business. That every block
  // was delivered, and that none had to be thrown away, is.
  const auto stats = capturer->stats();
  EXPECT_EQ(stats.blocks_delivered, after_a_second);
  EXPECT_EQ(stats.frames_dropped, 0U);
}

TEST(LoopbackCaptureTest, StoppingTwiceIsHarmless) {
  auto created = create_loopback_capturer([](std::span<const std::int16_t>) {}, [](dv::Error) {});
  ASSERT_TRUE(created.ok()) << created.error().message;
  auto capturer = std::move(created).take();

  const auto started = capturer->start(LoopbackMode::System, 0);
  if (!started.ok()) {
    GTEST_SKIP() << "the loopback would not start: " << started.error().message;
  }
  capturer->stop();
  capturer->stop();
  EXPECT_FALSE(capturer->capturing());
}

TEST(AudioSourcesTest, TheListNeverContainsThisProcess) {
  const auto found = audio_sources();
  if (!found.ok()) {
    GTEST_SKIP() << "the audio sessions could not be listed: " << found.error().message;
  }

  // Capturing our own output is the one thing this feature must never do, and
  // leaving this process out of the menu is where that is enforced.
  const auto self = static_cast<std::uint32_t>(GetCurrentProcessId());
  for (const auto& source : found.value()) {
    EXPECT_NE(source.process_id, self);
    EXPECT_NE(source.process_id, 0U);
    EXPECT_FALSE(source.name.empty());
  }

  // Whatever is playing sorts before whatever is not, so the menu opens on
  // what the user can actually hear.
  bool seen_quiet = false;
  for (const auto& source : found.value()) {
    if (!source.playing) {
      seen_quiet = true;
    } else {
      EXPECT_FALSE(seen_quiet) << "a playing source came after a quiet one";
    }
  }
}

#else  // defined(_WIN32)

TEST(LoopbackCaptureTest, EveryOtherPlatformSaysSoRatherThanSharingSilence) {
  EXPECT_FALSE(loopback_capture_is_available());

  const auto created =
      create_loopback_capturer([](std::span<const std::int16_t>) {}, [](dv::Error) {});
  ASSERT_FALSE(created.ok());
  EXPECT_EQ(created.error().code, "capture_unavailable");

  const auto found = audio_sources();
  ASSERT_FALSE(found.ok());
  EXPECT_EQ(found.error().code, "capture_unavailable");
}

#endif  // defined(_WIN32)

}  // namespace
