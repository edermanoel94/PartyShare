#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include <api/audio/audio_frame.h>
#include <api/audio/audio_frame_processor.h>
#include <api/audio/channel_layout.h>

#include "audio/screen_audio_mixer.hpp"

namespace dv::client::media {

/// Hands every captured frame to the screen audio mixer on its way to the
/// encoder.
///
/// libwebrtc calls this after the audio processing module and off the real time
/// capture path, which is what makes it the right seam: the echo canceller, the
/// noise suppressor and the gain control have already finished with the
/// microphone, so music mixed in here never meets any of them. See
/// docs/audio-da-tela-compartilhada.md, section 4, and the spike in
/// tools/screen_audio_spike that measured it rather than assuming it.
///
/// Thin on purpose. Everything that could be got wrong - gain, saturation,
/// channel counts, what to do when the capture starves - is in
/// audio::ScreenAudioMixer, where it can be tested without a call.
///
/// A header rather than a translation unit so that the spike can install the
/// same adapter the product installs. Proving the mixing with a copy of it
/// would prove something about the copy.
class ScreenAudioFrameProcessor final : public webrtc::AudioFrameProcessor {
 public:
  explicit ScreenAudioFrameProcessor(audio::ScreenAudioMixer* mixer) : mixer_(mixer) {}

  void Process(std::unique_ptr<webrtc::AudioFrame> frame) override {
    const std::size_t frames = frame->samples_per_channel();
    const std::size_t channels = frame->num_channels();

    if (frames == 0 || channels == 0) {
      deliver(std::move(frame));
      return;
    }

    // A muted frame reads as a buffer of zeros, which is what a silent
    // microphone is. Copying it out costs nothing and keeps one code path.
    microphone_.assign(frame->data(), frame->data() + (frames * channels));
    mixed_.assign(frames * 2, 0);

    const audio::MixResult result = mixer_->mix(microphone_, channels, mixed_);

    // The layout goes first, because the writable view is what records the
    // channel count the frame will be read with.
    frame->SetLayoutAndNumChannels(
        result.channels == 2 ? webrtc::CHANNEL_LAYOUT_STEREO : webrtc::CHANNEL_LAYOUT_MONO,
        result.channels);
    auto view = frame->mutable_data(frames, result.channels);
    const std::size_t count = frames * result.channels;
    for (std::size_t i = 0; i < count && i < mixed_.size(); ++i) {
      view[i] = mixed_[i];
    }

    deliver(std::move(frame));
  }

  void SetSink(OnAudioFrameCallback sink) override {
    const std::lock_guard<std::mutex> lock(mutex_);
    sink_ = std::move(sink);
  }

 private:
  void deliver(std::unique_ptr<webrtc::AudioFrame> frame) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (sink_) {
      sink_(std::move(frame));
    }
  }

  audio::ScreenAudioMixer* const mixer_;
  /// Reused rather than allocated per frame: this runs a hundred times a second
  /// for the length of every call.
  std::vector<std::int16_t> microphone_;
  std::vector<std::int16_t> mixed_;
  std::mutex mutex_;
  OnAudioFrameCallback sink_;
};

}  // namespace dv::client::media
