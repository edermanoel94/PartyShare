#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include <api/audio/audio_frame.h>
#include <api/audio/audio_frame_processor.h>
#include <api/audio/audio_view.h>
#include <api/audio/channel_layout.h>
#include <common_audio/resampler/include/push_resampler.h>

#include "audio/screen_audio_mixer.hpp"

namespace dv::client::media {

/// Hands every captured frame to the screen audio mixer on its way to the
/// encoder.
///
/// libwebrtc calls this after the audio processing module and off the real time
/// capture path, which is what makes it the right seam: the echo canceller, the
/// noise suppressor and the gain control have already finished with the
/// microphone, so music mixed in here never meets any of them. See
/// docs/09-screen-audio.md, section 1, and the spike in
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
    const int rate = frame->sample_rate_hz();

    if (frames == 0 || channels == 0 || rate <= 0) {
      deliver(std::move(frame));
      return;
    }

    // What the device is delivering, before anything is done to it. This is
    // where a 16 kHz headset becomes visible: the log says it once, and the
    // settings dialog tells the person what to do about it.
    mixer_->note_microphone_format(rate, channels);

    // A muted frame reads as a buffer of zeros, which is what a silent
    // microphone is. Copying it out costs nothing and keeps one code path.
    microphone_.assign(frame->data(), frame->data() + (frames * channels));

    // The microphone does not necessarily arrive at 48 kHz. The audio
    // processing module hands over whatever the capture device produces, and a
    // headset in communications mode produces 16 kHz - measured on this
    // project, and it is what silently stopped the mixing: 160 frames where
    // audio::kFramesPerBlock is 480.
    //
    // So the microphone is brought up to the rate the screen audio is already
    // at, rather than the screen audio being brought down to the microphone's.
    // Music at 16 kHz has nothing above 8 kHz left in it, and the whole point
    // of sending it is that it is not a voice.
    const std::size_t block = audio::kFramesPerBlock;
    if (frames != block || channels != 1) {
      to_mix_.assign(block, 0);
      resample(microphone_, frames, channels, to_mix_);
    } else {
      to_mix_ = microphone_;
    }

    mixed_.assign(block * 2, 0);
    const audio::MixResult result = mixer_->mix(to_mix_, 1, mixed_);

    // The rate and the layout go first: the writable view is what records the
    // shape the frame will be read with, and the encoder resamples from
    // whatever it is told to whatever Opus wants.
    frame->SetSampleRateAndChannelSize(audio::kSampleRateHz);
    frame->SetLayoutAndNumChannels(
        result.channels == 2 ? webrtc::CHANNEL_LAYOUT_STEREO : webrtc::CHANNEL_LAYOUT_MONO,
        result.channels);
    auto view = frame->mutable_data(block, result.channels);
    const std::size_t count = block * result.channels;
    for (std::size_t i = 0; i < count && i < mixed_.size(); ++i) {
      view[i] = mixed_[i];
    }

    deliver(std::move(frame));
  }

 private:
  /// Folds `channels` down to mono and resamples to one block at 48 kHz.
  void resample(const std::vector<std::int16_t>& in, std::size_t frames, std::size_t channels,
                std::vector<std::int16_t>& out) {
    mono_.assign(frames, 0);
    if (channels == 1) {
      std::copy(in.begin(), in.begin() + static_cast<std::ptrdiff_t>(frames), mono_.begin());
    } else {
      for (std::size_t frame = 0; frame < frames; ++frame) {
        std::int32_t sum = 0;
        for (std::size_t channel = 0; channel < channels; ++channel) {
          sum += in[(frame * channels) + channel];
        }
        mono_[frame] = static_cast<std::int16_t>(sum / static_cast<std::int32_t>(channels));
      }
    }
    resampler_.Resample(webrtc::MonoView<const std::int16_t>(mono_.data(), mono_.size()),
                        webrtc::MonoView<std::int16_t>(out.data(), out.size()));
  }

 public:
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
  /// The microphone as the mixer wants it: one block, mono, 48 kHz.
  std::vector<std::int16_t> to_mix_;
  std::vector<std::int16_t> mono_;
  std::vector<std::int16_t> mixed_;
  webrtc::PushResampler<std::int16_t> resampler_;
  std::mutex mutex_;
  OnAudioFrameCallback sink_;
};

}  // namespace dv::client::media
