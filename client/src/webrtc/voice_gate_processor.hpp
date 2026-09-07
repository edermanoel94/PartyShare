#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <api/audio/audio_processing.h>

#include "audio/voice_gate.hpp"
#include "media/media_session.hpp"

namespace dv::client::media {

/// Says whether a block of the microphone has a voice in it.
///
/// An interface so that the processor below can be tested with a detector that
/// says what the test tells it to. The one that ships is libwebrtc's, from
/// make_webrtc_voice_detector.
class VoiceDetector {
 public:
  VoiceDetector() = default;
  VoiceDetector(const VoiceDetector&) = delete;
  VoiceDetector& operator=(const VoiceDetector&) = delete;
  VoiceDetector(VoiceDetector&&) = delete;
  VoiceDetector& operator=(VoiceDetector&&) = delete;
  virtual ~VoiceDetector() = default;

  /// Prepares for blocks of `frames` samples at `sample_rate_hz`. False when
  /// that is a format this detector cannot judge, in which case `voiced` is
  /// never called.
  virtual bool initialize(int sample_rate_hz, std::size_t frames) = 0;
  /// How sure the detector has to be. Called on the thread that calls `voiced`.
  virtual void set_level(VoiceGateLevel level) = 0;
  /// The word on one block.
  virtual bool voiced(const std::int16_t* samples, std::size_t frames) = 0;
};

/// libwebrtc's own voice activity detector, the one its codecs and its first
/// gain controller use: a pair of Gaussian mixtures over six bands between
/// 80 Hz and 4 kHz, with a noise model that follows the room. Its four modes
/// are the gate's four levels. It judges 8, 16, 32 and 48 kHz in 10 ms blocks,
/// which is exactly what the processing module hands out, so nothing is
/// resampled on the way.
[[nodiscard]] std::unique_ptr<VoiceDetector> make_webrtc_voice_detector();

/// The voice gate, as a capture post-processor of the audio processing module.
///
/// It sits at the very end of the capture chain - after the echo canceller,
/// the suppressor and the gain control have each had their say, and before the
/// screen sound is mixed in - which is the right place for it twice over. After
/// the gain control, because the detector then judges a signal that has already
/// been levelled, and a quiet speaker is not a closed gate. Before the mixer,
/// because a share's sound goes through the mixer and not through here, so a
/// film is never gated for sounding nothing like a voice.
///
/// The word on each block comes from the detector; the gain that follows from
/// it is audio::VoiceGate's. `configure` may be called from any thread and takes
/// effect on the next block: the switch and the level are atomics that Process
/// reads, and the detector's mode is moved there, on the thread that uses it.
class VoiceGateProcessor final : public webrtc::CustomProcessing {
 public:
  /// What the gate is doing, for the statistics.
  struct State {
    bool enabled = true;
    VoiceGateLevel level = VoiceGateLevel::Moderate;
    /// Whether the last block was let through.
    bool open = true;
    /// Blocks judged since the format was last set, and how many of them the
    /// gate was closed for. Zero blocks with the gate on means it is not
    /// running: no capture yet, or a format the detector cannot judge.
    std::uint64_t blocks = 0;
    std::uint64_t closed_blocks = 0;
  };

  explicit VoiceGateProcessor(
      std::unique_ptr<VoiceDetector> detector = make_webrtc_voice_detector());
  VoiceGateProcessor(const VoiceGateProcessor&) = delete;
  VoiceGateProcessor& operator=(const VoiceGateProcessor&) = delete;
  VoiceGateProcessor(VoiceGateProcessor&&) = delete;
  VoiceGateProcessor& operator=(VoiceGateProcessor&&) = delete;
  ~VoiceGateProcessor() override;

  /// Switches the gate and sets its level, from any thread.
  void configure(bool enabled, VoiceGateLevel level);
  [[nodiscard]] State state() const;

  // webrtc::CustomProcessing, called by the module under its capture lock.
  void Initialize(int sample_rate_hz, int num_channels) override;
  void Process(webrtc::AudioBuffer* audio) override;
  [[nodiscard]] std::string ToString() const override;

 private:
  // The gate's own book-keeping between blocks, all of it under the module's
  // capture lock. Everything another thread may read is an atomic below.
  std::unique_ptr<VoiceDetector> detector_;
  audio::VoiceGate gate_;
  std::vector<std::int16_t> pcm_;
  int sample_rate_hz_ = 0;
  std::size_t frames_ = 0;
  bool detector_ready_ = false;
  bool was_enabled_ = true;
  std::optional<VoiceGateLevel> applied_level_;

  std::atomic<bool> enabled_{true};
  std::atomic<std::uint8_t> level_{static_cast<std::uint8_t>(VoiceGateLevel::Moderate)};
  std::atomic<bool> open_{true};
  std::atomic<std::uint64_t> blocks_{0};
  std::atomic<std::uint64_t> closed_blocks_{0};
};

}  // namespace dv::client::media
