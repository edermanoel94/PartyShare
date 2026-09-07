#include "webrtc/voice_gate_processor.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

#include <common_audio/vad/include/webrtc_vad.h>
#include <modules/audio_processing/audio_buffer.h>

#include <dv/logging/logger.hpp>

namespace dv::client::media {
namespace {

/// The detector's four modes, 0 to 3, in its own words: quality, low bitrate,
/// aggressive, very aggressive. Higher is stricter: it says "voice" less often,
/// and misses more of the quiet ones.
[[nodiscard]] int to_vad_mode(VoiceGateLevel level) noexcept {
  switch (level) {
    case VoiceGateLevel::Low:
      return 0;
    case VoiceGateLevel::Moderate:
      return 1;
    case VoiceGateLevel::High:
      return 2;
    case VoiceGateLevel::VeryHigh:
      return 3;
  }
  return 1;
}

/// The rates the detector judges, always in 10 ms blocks, which is one hundredth
/// of the rate. Checked here rather than asked of the library, whose answer to
/// a pair it does not take is -1 from every Process call: a gate that never
/// opens, with nothing in the log to say why.
[[nodiscard]] bool detector_accepts(int sample_rate_hz, std::size_t frames) noexcept {
  const bool rate_ok = sample_rate_hz == 8000 || sample_rate_hz == 16000 ||
                       sample_rate_hz == 32000 || sample_rate_hz == 48000;
  return rate_ok && frames == static_cast<std::size_t>(sample_rate_hz / 100);
}

class WebrtcVoiceDetector final : public VoiceDetector {
 public:
  bool initialize(int sample_rate_hz, std::size_t frames) override {
    // Init puts the mode back to the default along with the rest of the
    // state, so the level has to be applied again after it. The processor
    // does that by forgetting which level it applied.
    if (vad_ == nullptr || WebRtcVad_Init(vad_.get()) != 0) {
      return false;
    }
    sample_rate_hz_ = sample_rate_hz;
    return detector_accepts(sample_rate_hz, frames);
  }

  void set_level(VoiceGateLevel level) override {
    if (vad_ != nullptr && WebRtcVad_set_mode(vad_.get(), to_vad_mode(level)) != 0) {
      DV_LOG_WARN("Voice gate: the detector refused level {}", to_string(level));
    }
  }

  bool voiced(const std::int16_t* samples, std::size_t frames) override {
    return vad_ != nullptr && WebRtcVad_Process(vad_.get(), sample_rate_hz_, samples, frames) == 1;
  }

 private:
  std::unique_ptr<VadInst, void (*)(VadInst*)> vad_{WebRtcVad_Create(), &WebRtcVad_Free};
  int sample_rate_hz_ = 0;
};

}  // namespace

std::unique_ptr<VoiceDetector> make_webrtc_voice_detector() {
  return std::make_unique<WebrtcVoiceDetector>();
}

VoiceGateProcessor::VoiceGateProcessor(std::unique_ptr<VoiceDetector> detector)
    : detector_(std::move(detector)) {}

VoiceGateProcessor::~VoiceGateProcessor() = default;

void VoiceGateProcessor::configure(bool enabled, VoiceGateLevel level) {
  // The level first, so that a block which sees the switch also sees the
  // level that came with it.
  level_.store(static_cast<std::uint8_t>(level), std::memory_order_relaxed);
  enabled_.store(enabled, std::memory_order_release);
}

VoiceGateProcessor::State VoiceGateProcessor::state() const {
  return State{
      .enabled = enabled_.load(std::memory_order_acquire),
      .level = static_cast<VoiceGateLevel>(level_.load(std::memory_order_relaxed)),
      .open = open_.load(std::memory_order_relaxed),
      .blocks = blocks_.load(std::memory_order_relaxed),
      .closed_blocks = closed_blocks_.load(std::memory_order_relaxed),
  };
}

void VoiceGateProcessor::Initialize(int sample_rate_hz, int /*num_channels*/) {
  sample_rate_hz_ = sample_rate_hz;
  frames_ = sample_rate_hz > 0 ? static_cast<std::size_t>(sample_rate_hz / 100) : std::size_t{0};
  pcm_.assign(frames_, 0);
  detector_ready_ =
      detector_ != nullptr && frames_ > 0 && detector_->initialize(sample_rate_hz, frames_);
  applied_level_.reset();
  gate_.reset(sample_rate_hz);
  open_.store(true, std::memory_order_relaxed);
  blocks_.store(0, std::memory_order_relaxed);
  closed_blocks_.store(0, std::memory_order_relaxed);
  if (!detector_ready_) {
    DV_LOG_WARN(
        "Voice gate: no detector for {} Hz in blocks of {}; the microphone goes through ungated",
        sample_rate_hz, frames_);
  }
}

void VoiceGateProcessor::Process(webrtc::AudioBuffer* audio) {
  const bool enabled = enabled_.load(std::memory_order_acquire);
  if (!enabled || !detector_ready_ || audio == nullptr) {
    // Straight through. The envelope is forgotten on the way out, so that
    // switching the gate back on starts it open with a full hold, and not
    // wherever it happened to be when it was switched off.
    if (!enabled && was_enabled_) {
      gate_.reset(sample_rate_hz_);
    }
    was_enabled_ = enabled;
    open_.store(true, std::memory_order_relaxed);
    return;
  }
  was_enabled_ = true;

  const std::size_t frames = audio->num_frames();
  const std::size_t num_channels = audio->num_channels();
  if (frames != frames_ || num_channels == 0) {
    open_.store(true, std::memory_order_relaxed);
    return;
  }

  const auto level = static_cast<VoiceGateLevel>(level_.load(std::memory_order_relaxed));
  if (applied_level_ != level) {
    detector_->set_level(level);
    applied_level_ = level;
  }

  // The module keeps its floats on the 16 bit scale, so the conversion is a
  // rounding and a clamp and not a rescale. The first channel is the one
  // judged: the capture is mono, and a second channel of the same microphone
  // would only say the same thing again.
  const float* first = audio->channels_const()[0];
  for (std::size_t i = 0; i < frames; ++i) {
    pcm_[i] = static_cast<std::int16_t>(std::lround(std::clamp(first[i], -32768.0F, 32767.0F)));
  }
  const bool voiced = detector_->voiced(pcm_.data(), frames);
  gate_.apply(voiced, audio->channels(), num_channels, frames);

  open_.store(gate_.open(), std::memory_order_relaxed);
  blocks_.store(gate_.blocks(), std::memory_order_relaxed);
  closed_blocks_.store(gate_.closed_blocks(), std::memory_order_relaxed);
}

std::string VoiceGateProcessor::ToString() const {
  const State current = state();
  return std::string("VoiceGate{enabled=") + (current.enabled ? "true" : "false") +
         ", level=" + std::string(to_string(current.level)) +
         ", open=" + (current.open ? "true" : "false") + "}";
}

}  // namespace dv::client::media
