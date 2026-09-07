#include "audio/voice_gate.hpp"

#include <algorithm>

namespace dv::client::audio {
namespace {

/// Samples in `ms` at `sample_rate_hz`, and never fewer than one: a step of one
/// over zero samples is a division by zero, and a ramp of zero samples is the
/// click the ramp exists to avoid.
[[nodiscard]] std::size_t samples_in(int ms, int sample_rate_hz) noexcept {
  const long long samples = static_cast<long long>(ms) * sample_rate_hz / 1000;
  return samples > 0 ? static_cast<std::size_t>(samples) : std::size_t{1};
}

}  // namespace

VoiceGate::VoiceGate(VoiceGateTiming timing) : timing_(timing) {
  reset(48000);
}

void VoiceGate::reset(int sample_rate_hz) {
  hold_samples_ = samples_in(timing_.hold_ms, sample_rate_hz);
  attack_step_ = 1.0F / static_cast<float>(samples_in(timing_.attack_ms, sample_rate_hz));
  release_step_ = 1.0F / static_cast<float>(samples_in(timing_.release_ms, sample_rate_hz));
  hold_remaining_ = hold_samples_;
  gain_ = 1.0F;
  open_ = true;
  blocks_ = 0;
  closed_blocks_ = 0;
}

void VoiceGate::apply(bool voiced, float* const* channels, std::size_t num_channels,
                      std::size_t num_frames) {
  ++blocks_;

  // The hold is measured in silence: every block with a voice in it starts it
  // over, and every block without one uses some of it up.
  if (voiced) {
    hold_remaining_ = hold_samples_;
  } else if (hold_remaining_ > num_frames) {
    hold_remaining_ -= num_frames;
  } else {
    hold_remaining_ = 0;
  }
  open_ = voiced || hold_remaining_ > 0;
  if (!open_) {
    ++closed_blocks_;
  }

  // Settled at the end it is heading for: nothing to ramp. Open, the block is
  // left exactly as it came, which is the common case during a sentence and
  // costs nothing. Closed, it is silence.
  const bool settled = open_ ? gain_ >= 1.0F : gain_ <= 0.0F;
  if (settled) {
    if (!open_) {
      for (std::size_t channel = 0; channel < num_channels; ++channel) {
        std::fill_n(channels[channel], num_frames, 0.0F);
      }
    }
    return;
  }

  const float step = open_ ? attack_step_ : -release_step_;
  for (std::size_t i = 0; i < num_frames; ++i) {
    gain_ = std::clamp(gain_ + step, 0.0F, 1.0F);
    for (std::size_t channel = 0; channel < num_channels; ++channel) {
      channels[channel][i] *= gain_;
    }
  }
}

}  // namespace dv::client::audio
