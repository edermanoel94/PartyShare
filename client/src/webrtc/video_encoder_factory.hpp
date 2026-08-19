// The factory the media engine hands to libwebrtc.
//
// It is one line of policy: use the hardware encoder when there is one, and
// the software encoder when there is not, with libwebrtc's own fallback
// wrapper in between so that hardware which accepts a stream and then fails
// halfway does not take the call down with it.

#pragma once

#include <memory>

#include <api/video_codecs/video_encoder_factory.h>

namespace dv::client::media {

/// `prefer_hardware` false is the escape hatch: it forces software even on a
/// machine with a working card, which is what a user with a driver that
/// misbehaves needs, and what a benchmark comparing the two needs.
[[nodiscard]] std::unique_ptr<webrtc::VideoEncoderFactory> create_video_encoder_factory(
    bool prefer_hardware);

}  // namespace dv::client::media
