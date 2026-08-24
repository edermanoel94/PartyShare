// One hardware encoder implementation, as the chain sees it.
//
// Internal to the media layer. The seam everything above uses is
// webrtc/hardware_encoder.hpp, and it does not change: this header exists so
// that one platform can have more than one backend, which Windows does. There
// NVENC covers NVIDIA and Media Foundation covers Intel and AMD, and which one
// a machine gets is a question that can only be answered by asking both.
//
// Selected at runtime rather than at build time, because a build has to run on
// machines with different cards in them.

#pragma once

#include <memory>
#include <string>

#include <api/environment/environment.h>
#include <api/video_codecs/sdp_video_format.h>
#include <api/video_codecs/video_encoder.h>

namespace dv::client::media {

/// What one backend answers when asked whether this machine can encode.
struct HardwareEncoderProbe {
  bool available = false;
  /// Why, in either direction, in words meant for whoever is wondering why
  /// their card is idle. Never empty: see hardware_encoder.hpp.
  std::string detail;
};

struct HardwareEncoderBackend {
  /// For the log and for HardwareEncoderSupport::implementation: "NVENC",
  /// "Media Foundation".
  const char* name = "";
  /// What DV_HARDWARE_ENCODER accepts for this one: "nvenc",
  /// "mediafoundation". Separate from `name` so the environment variable does
  /// not depend on how the display name is spelled or spaced.
  const char* slug = "";

  /// Asked once per process. The answer cannot change while it runs: a card
  /// does not appear, and a driver that mismatches its own kernel module keeps
  /// mismatching until the machine is rebooted.
  HardwareEncoderProbe (*probe)() = nullptr;

  /// nullptr when this backend does not do `format`, which is not an error.
  std::unique_ptr<webrtc::VideoEncoder> (*create)(const webrtc::Environment& env,
                                                  const webrtc::SdpVideoFormat& format) = nullptr;
};

#ifdef DV_WITH_NVENC
[[nodiscard]] HardwareEncoderBackend nvenc_backend();
#endif

#ifdef DV_WITH_MEDIA_FOUNDATION
[[nodiscard]] HardwareEncoderBackend media_foundation_backend();
#endif

}  // namespace dv::client::media
