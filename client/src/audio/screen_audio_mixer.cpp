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

/// Unity in the fixed point the screen gain is kept in. See
/// ScreenAudioMixer::screen_gain_.
constexpr std::int32_t kGainOne = 256;
constexpr int kGainShift = 8;

/// One sample at the screen gain, still wide enough to be added to a microphone
/// before anything is clamped.
///
/// The shift is arithmetic on a signed left operand from C++20 on, which is
/// what this needs: a negative half of a waveform quietened by a right shift
/// that filled with zeros would come back as a positive spike.
[[nodiscard]] std::int32_t at_gain(std::int16_t sample, std::int32_t gain) noexcept {
  return (static_cast<std::int32_t>(sample) * gain) >> kGainShift;
}

}  // namespace

double screen_volume_ratio(int percent) noexcept {
  const int asked = std::clamp(percent, 0, kMaxScreenVolumePercent);

  // Linear: the ratio is the percentage. Half on the slider is half the
  // amplitude of the samples.
  //
  // This is the arithmetic answer rather than the perceptual one, and the two
  // disagree: loudness follows roughly the cube root of amplitude, so half the
  // amplitude is heard as about four fifths as loud, and a slider calibrated
  // this way does most of its audible work in the bottom quarter of its travel.
  // A perceptual curve - the ratio raised to a power, or decibels - spreads the
  // change evenly along the slider instead.
  //
  // Left linear because this control is not a listening volume: it balances the
  // screen against a microphone, and the number people will reach for is a
  // ratio between two sources - "half as loud as me" - rather than a position
  // that feels halfway. It is also the one mapping where the reading on screen
  // and the arithmetic in the mix are the same claim, which matters when
  // somebody is trying to work out why a share is quiet.
  return static_cast<double>(asked) / 100.0;
}

void ScreenAudioMixer::set_screen_volume(int percent) noexcept {
  const int asked = std::clamp(percent, 0, kMaxScreenVolumePercent);
  screen_volume_percent_.store(asked);
  screen_gain_.store(static_cast<std::int32_t>(std::lround(screen_volume_ratio(asked) * kGainOne)));
}

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

  // Read once for the whole block rather than per sample. A slider moved while
  // this runs then lands between blocks instead of inside one, which is the
  // difference between a volume change and a step in the middle of a waveform.
  const std::int32_t screen_gain = screen_gain_.load();

  for (std::size_t frame = 0; frame < frames; ++frame) {
    // A mono microphone goes to both ears; a stereo one keeps its sides. The
    // second case does not happen today - the capture pipeline is mono, as
    // section 9 of SPEC.md asks - but reading channel zero and calling it
    // stereo would be a lie waiting to be found.
    const std::int32_t left = static_cast<std::int32_t>(microphone[frame * channels]) * gain;
    const std::int32_t right =
        static_cast<std::int32_t>(microphone[(frame * channels) + (channels - 1)]) * gain;

    // The gain lands on the screen audio alone. The microphone keeps whatever
    // libwebrtc's own gain control left it at, which is the level the far end
    // has already learned this person's voice at.
    out[frame * 2] = saturate(left + at_gain(screen_block_[frame * 2], screen_gain));
    out[(frame * 2) + 1] = saturate(right + at_gain(screen_block_[(frame * 2) + 1], screen_gain));
  }

  return result;
}

}  // namespace dv::client::audio
