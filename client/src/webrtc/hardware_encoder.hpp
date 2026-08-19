// The seam a hardware video encoder plugs into.
//
// There is no VideoEncoder interface of our own: libwebrtc already has one,
// and it is what the encoder factory hands out. What this header adds is the
// question "does this machine have hardware that can encode H.264", answered
// per platform, plus the constructor for it.
//
// Section 6 of SPEC.md and task 4 of M8 ask for Media Foundation or NVENC on
// Windows, VideoToolbox on macOS and VAAPI on Linux. Each is a separate
// implementation of the two functions below, selected at build time. A build
// with none of them still compiles and still encodes, in software.

#pragma once

#include <memory>
#include <string>

#include <api/environment/environment.h>
#include <api/video_codecs/sdp_video_format.h>
#include <api/video_codecs/video_encoder.h>

namespace dv::client::media {

/// What this build was compiled with and what this machine can actually do,
/// which are different questions: a binary built with NVENC still has to run
/// on a machine that has the card, the driver, and a driver that matches its
/// own kernel module.
struct HardwareEncoderSupport {
  /// True when this build contains a hardware backend at all.
  bool compiled_in = false;
  /// True when that backend found usable hardware here and now.
  bool available = false;
  /// What the backend is called, for the log: "NVENC", "VAAPI", and so on.
  /// Empty when none was compiled in.
  std::string implementation;
  /// Why it is not available, when it is not. Meant to be read by a person
  /// wondering why their card is idle.
  std::string detail;
};

/// Asked once per process, and cheap after that.
[[nodiscard]] HardwareEncoderSupport hardware_encoder_support();

/// Creates a hardware encoder for `format`, or nullptr when this machine or
/// this build has none, or when the format is not one the hardware does.
///
/// A null return is not an error: it means the software encoder is used, which
/// is what happens on most machines.
[[nodiscard]] std::unique_ptr<webrtc::VideoEncoder> create_hardware_encoder(
    const webrtc::Environment& env, const webrtc::SdpVideoFormat& format);

}  // namespace dv::client::media
