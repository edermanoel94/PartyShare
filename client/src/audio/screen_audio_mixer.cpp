#include "audio/screen_audio_mixer.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

#include <dv/logging/logger.hpp>

namespace dv::client::audio {

namespace {

/// The capture already hands out one block every 10 ms, so this second buffer
/// only has to absorb the jitter between two clocks that run at the same rate.
/// Small on purpose: every millisecond held here is a millisecond the shared
/// video is ahead of its own sound.
constexpr BlockPacer::Options kPacing{
    .high_watermark_frames = static_cast<std::size_t>(kSampleRateHz) / 16,  // 60 ms
    .target_frames = static_cast<std::size_t>(kSampleRateHz) / 32,          // 30 ms
    .prime_frames = static_cast<std::size_t>(kSampleRateHz) / 50,           // 20 ms
};

[[nodiscard]] std::int16_t saturate(std::int32_t sample) noexcept {
  constexpr std::int32_t kMax = 32767;
  constexpr std::int32_t kMin = -32768;
  return static_cast<std::int16_t>(std::clamp(sample, kMin, kMax));
}

}  // namespace

ScreenAudioMixer::ScreenAudioMixer() : pacer_(kPacing), screen_block_(kSamplesPerBlock, 0) {}

ScreenAudioMixer::~ScreenAudioMixer() {
  stop();
}

Result<std::monostate> ScreenAudioMixer::start(LoopbackMode mode, std::uint32_t process_id) {
  stop();

  {
    const std::lock_guard<std::mutex> lock(failure_mutex_);
    failure_ = {};
  }
  pacer_.clear();

  auto created = create_loopback_capturer(
      [this](std::span<const std::int16_t> block) { push_screen_audio(block); },
      [this](Error error) { note_failure(std::move(error)); });
  if (!created) {
    return Result<std::monostate>::failure(created.error());
  }
  capturer_ = std::move(created).take();

  if (auto started = capturer_->start(mode, process_id); !started) {
    capturer_.reset();
    return started;
  }

  active_ = true;
  return std::monostate{};
}

void ScreenAudioMixer::stop() {
  active_ = false;
  if (capturer_ != nullptr) {
    capturer_->stop();
    capturer_.reset();
  }
  pacer_.clear();
}

Error ScreenAudioMixer::failure() const {
  const std::lock_guard<std::mutex> lock(failure_mutex_);
  return failure_;
}

LoopbackStats ScreenAudioMixer::capture_stats() const {
  // Read without a lock: the pointer is only replaced under start and stop,
  // both of which are called from whoever drives the share, never from the
  // audio path.
  return capturer_ == nullptr ? LoopbackStats{} : capturer_->stats();
}

void ScreenAudioMixer::note_failure(Error error) {
  DV_LOG_WARN("Screen audio: the capture ended: {}", error.message);
  active_ = false;
  const std::lock_guard<std::mutex> lock(failure_mutex_);
  failure_ = std::move(error);
}

MixResult ScreenAudioMixer::mix(std::span<const std::int16_t> microphone, std::size_t channels,
                                std::span<std::int16_t> out) {
  MixResult result;
  if (channels == 0 || microphone.empty()) {
    return result;
  }

  const std::size_t frames = microphone.size() / channels;

  // The microphone on its own, before its gain and before anything is added.
  double energy = 0;
  for (const std::int16_t sample : microphone) {
    const double value = static_cast<double>(sample) / 32768.0;
    energy += value * value;
  }
  result.microphone_level = std::sqrt(energy / static_cast<double>(microphone.size()));
  microphone_level_.store(result.microphone_level);

  const std::int32_t gain = microphone_muted_.load() ? 0 : 1;

  // A running capture keeps the output stereo even while it starves, so the
  // channel count does not flap every time an application goes quiet. Buffered
  // audio with no capture behind it is what a test pushes by hand.
  const bool have_screen_audio = active_.load() || pacer_.buffered_frames() > 0;
  const bool mixable = have_screen_audio && frames == kFramesPerBlock &&
                       out.size() >= kSamplesPerBlock && channels <= 2;
  if (have_screen_audio && !mixable && !warned_about_frame_size_.exchange(true)) {
    DV_LOG_WARN(
        "Screen audio: not mixing, because a captured frame is {} frames of {} channels and this "
        "expects {} of at most two",
        frames, channels, kFramesPerBlock);
  }

  if (!mixable) {
    // Straight through, keeping the channel count it arrived with. The gain
    // still applies: muting has to work whether or not a share is on.
    const std::size_t count = std::min(microphone.size(), out.size());
    for (std::size_t i = 0; i < count; ++i) {
      out[i] = saturate(static_cast<std::int32_t>(microphone[i]) * gain);
    }
    result.channels = channels;
    return result;
  }

  result.screen_audio = pacer_.take(screen_block_);
  result.channels = 2;

  for (std::size_t frame = 0; frame < frames; ++frame) {
    // A mono microphone goes to both ears; a stereo one keeps its sides. The
    // second case does not happen today - the capture pipeline is mono, as
    // section 9 of SPEC.md asks - but reading channel zero and calling it
    // stereo would be a lie waiting to be found.
    const std::int32_t left = static_cast<std::int32_t>(microphone[frame * channels]) * gain;
    const std::int32_t right =
        static_cast<std::int32_t>(microphone[(frame * channels) + (channels - 1)]) * gain;

    out[frame * 2] = saturate(left + screen_block_[frame * 2]);
    out[(frame * 2) + 1] = saturate(right + screen_block_[(frame * 2) + 1]);
  }

  return result;
}

}  // namespace dv::client::audio
