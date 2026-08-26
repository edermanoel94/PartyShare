#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include <dv/core/result.hpp>

#include "audio/block_pacer.hpp"
#include "audio/loopback_capturer.hpp"

namespace dv::client::audio {

/// The loudest the screen audio may be asked to go, as a percentage of what the
/// application itself is playing.
///
/// Above 100 clips, and `saturate` is what it clips against, so the worst a
/// boost can do is sound bad rather than wrap around into noise. It is offered
/// because the alternative is worse: an application playing into a tenth of its
/// scale has no other way back, and "go and turn Windows up" is not a volume
/// control.
constexpr int kMaxScreenVolumePercent = 200;

/// Turns the percentage the interface shows into the ratio `mix` multiplies the
/// screen audio by, before the microphone is added to it.
///
/// Separated from the mixer, and from the slider, because it is the one piece
/// of this feature with a judgement in it rather than a mechanism: it decides
/// what "half volume" means to somebody dragging a control, and neither the
/// widget nor the arithmetic has an opinion about that.
///
/// Anything outside 0 to `kMaxScreenVolumePercent` is clamped rather than
/// refused: this is reached from a saved configuration as well as from a
/// slider, and a file with 500 in it should be loud, not fatal.
[[nodiscard]] double screen_volume_ratio(int percent) noexcept;

/// What one call to `mix` produced.
struct MixResult {
  /// One while only the microphone is going out, two once the screen audio is.
  std::size_t channels = 1;
  /// The microphone on its own, from 0 to 1, measured before its gain and
  /// before anything was added to it.
  ///
  /// This is the only honest source for the speaking indicator once the screen
  /// audio is in the track: everything downstream sees voice and music
  /// together, so a level read from the encoder or from the RTP header would
  /// light up for the whole length of a video.
  double microphone_level = 0;
  /// True when a block of screen audio was actually mixed in, false while the
  /// capture is starving or stopped.
  bool screen_audio = false;
};

/// Adds what the machine is playing to what the microphone captured.
///
/// This is the Opção A of docs/audio-da-tela-compartilhada.md: rather than a
/// second track, a second connection and a second negotiation, the screen audio
/// rides in the participant's own audio track. The spike in
/// tools/screen_audio_spike proved the three things that makes possible - the
/// hook exists, it runs after the echo canceller and the noise suppressor, and
/// a stereo frame handed back at that point reaches the other end in stereo.
///
/// Running after the audio processing module is the whole point. Mixing music
/// in before it would hand a noise suppressor set to `kHigh` a signal it reads
/// as stationary noise, and an automatic gain control that pumps the volume at
/// every note.
///
/// This class holds no libwebrtc type on purpose. `mix` is arithmetic over two
/// buffers, which is what makes it testable without a sound card, a call or the
/// toolchain of docs/webrtc-toolchain.md. The adapter that turns it into a
/// `webrtc::AudioFrameProcessor` lives in
/// client/src/webrtc/libwebrtc_media_session.cpp.
///
/// Thread safe. `mix` is called from libwebrtc's audio processing thread while
/// the capture pushes from its own.
class ScreenAudioMixer {
 public:
  ScreenAudioMixer();
  ~ScreenAudioMixer();

  ScreenAudioMixer(const ScreenAudioMixer&) = delete;
  ScreenAudioMixer& operator=(const ScreenAudioMixer&) = delete;
  ScreenAudioMixer(ScreenAudioMixer&&) = delete;
  ScreenAudioMixer& operator=(ScreenAudioMixer&&) = delete;

  /// Starts capturing and mixing. Fails the way
  /// LoopbackCapturer::start fails, and changes nothing when it does.
  ///
  /// Starting again while already running restarts on the new source.
  [[nodiscard]] Result<std::monostate> start(LoopbackMode mode, std::uint32_t process_id);

  void stop();

  [[nodiscard]] bool active() const noexcept { return active_.load(); }

  /// Why the capture stopped on its own, empty when it did not.
  ///
  /// Polled rather than pushed: the mixer outlives every session in the process
  /// and handing it a callback owned by one of them is a lifetime problem for
  /// no gain, when whoever cares is already collecting statistics on a timer.
  [[nodiscard]] Error failure() const;

  [[nodiscard]] LoopbackStats capture_stats() const;

  /// What the buffer between the capture and the encoder did.
  ///
  /// A different question from capture_stats(), and the one that says whether
  /// the sound reached the track: the capture can be delivering perfectly while
  /// this starves, and then the share is silent for everybody.
  [[nodiscard]] BlockPacer::Stats mix_stats() const { return pacer_.stats(); }

  /// Silences the microphone without silencing the screen audio.
  ///
  /// This is why muting stopped being `track->set_enabled(false)` while a share
  /// is on: disabling the track would take the film with it. Zeroing the
  /// microphone here is also more exact - the track carries silence from the
  /// microphone rather than nothing at all from anywhere.
  void set_microphone_muted(bool muted) noexcept { microphone_muted_ = muted; }

  [[nodiscard]] bool microphone_muted() const noexcept { return microphone_muted_.load(); }

  /// How loud the screen audio goes out, as the percentage the interface shows.
  ///
  /// Only the screen's share of the mix moves. The microphone is deliberately
  /// left alone: it already has a gain control, and it is libwebrtc's, running
  /// before this code ever sees the samples. Two automatic gain controls
  /// fighting over one signal is not a volume setting.
  ///
  /// Takes effect on the next block, which is within 10 ms. There is no ramp,
  /// and at the speed a slider moves there does not need to be one: a drag
  /// arrives as many small steps rather than one jump, and it is the jump that
  /// would click.
  void set_screen_volume(int percent) noexcept;

  [[nodiscard]] int screen_volume() const noexcept { return screen_volume_percent_.load(); }

  /// The microphone level the last mixed block saw, from 0 to 1.
  [[nodiscard]] double microphone_level() const noexcept { return microphone_level_.load(); }

  /// Hands over screen audio to be mixed: interleaved stereo 16-bit at 48 kHz.
  ///
  /// This is the seam the capture pushes through, and it is public because it
  /// is also how the mixing is driven in a test, with no operating system
  /// underneath. Buffered rather than mixed on the spot: the capture's clock
  /// and the one libwebrtc pulls on are not the same clock.
  void push_screen_audio(std::span<const std::int16_t> block) { pacer_.push(block); }

  /// Writes `microphone` plus whatever the screen is playing into `out`.
  ///
  /// `microphone` holds `frames * channels` interleaved samples at 48 kHz.
  /// `out` has to be big enough for `frames * 2`, because the result is stereo
  /// whenever there is screen audio to put in it.
  ///
  /// A frame that is not exactly one block long passes through untouched: the
  /// audio processing module is configured for 48 kHz and hands over 10 ms at a
  /// time, so anything else means an assumption broke, and mixing on a guess
  /// would be worse than not mixing.
  MixResult mix(std::span<const std::int16_t> microphone, std::size_t channels,
                std::span<std::int16_t> out);

 private:
  void note_failure(Error error);

  std::unique_ptr<LoopbackCapturer> capturer_;
  /// Between the capture's clock and the one libwebrtc pulls on. Both run at a
  /// hundred blocks a second and neither is the other, which is exactly what
  /// BlockPacer is for.
  BlockPacer pacer_;
  std::vector<std::int16_t> screen_block_;

  std::atomic<bool> active_{false};
  std::atomic<bool> microphone_muted_{false};
  /// What was asked for, kept so the getter can answer in the same units the
  /// slider and config.ini use rather than in the mixer's own.
  std::atomic<int> screen_volume_percent_{100};
  /// The same thing as the multiplier the sample loop actually applies, in
  /// 8-bit fixed point: 256 is unity, 512 is the ceiling.
  ///
  /// Fixed point and not a double because this is read once per sample, a
  /// hundred blocks a second for the length of every call, and because the
  /// arithmetic beside it is already integer. Eight bits rather than sixteen so
  /// the product stays inside an int32: a full scale sample at the ceiling is
  /// 32767 * 512, which fits, where the same in Q16 would not. A step of 1/256
  /// is finer than the one percent the slider moves in, so nothing is lost
  /// rounding into it.
  std::atomic<std::int32_t> screen_gain_{256};
  std::atomic<double> microphone_level_{0};
  std::atomic<bool> warned_about_frame_size_{false};

  mutable std::mutex failure_mutex_;
  Error failure_;
};

}  // namespace dv::client::audio
