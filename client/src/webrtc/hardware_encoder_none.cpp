// The hardware encoder seam on a build with no backend for this platform.
//
// Compiled whenever DV_HARDWARE_ENCODER is off or the platform has no
// implementation yet, which today is every platform but Linux with NVENC.
// Everything above it works unchanged: the factory asks, gets nothing, and
// hands out the software encoder.

#include "webrtc/hardware_encoder.hpp"

#ifndef DV_WITH_NVENC

namespace dv::client::media {

HardwareEncoderSupport hardware_encoder_support() {
  return HardwareEncoderSupport{
      .compiled_in = false,
      .available = false,
      .implementation = {},
      .detail = "this build has no hardware encoder backend for this platform",
  };
}

std::unique_ptr<webrtc::VideoEncoder> create_hardware_encoder(
    const webrtc::Environment& /*env*/, const webrtc::SdpVideoFormat& /*format*/) {
  return nullptr;
}

}  // namespace dv::client::media

#endif  // DV_WITH_NVENC
