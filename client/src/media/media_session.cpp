#include "media/media_session.hpp"

namespace dv::client::media {

#ifndef DV_WITH_LIBWEBRTC

// Build without libwebrtc. Everything above this interface still compiles and
// runs, which is what lets the tests and the CI matrix work without a 66 MB
// static library that has to be built from source.
//
// The counterpart lives in client/src/webrtc/libwebrtc_media_session.cpp.

// The callbacks are taken by value because the real implementation moves them
// into the session. This one has nowhere to put them, and cannot change the
// signature it is standing in for.
// NOLINTBEGIN(performance-unnecessary-value-param)
Result<std::unique_ptr<MediaSession>> create_media_session(const MediaSessionOptions& /*options*/,
                                                           MediaSession::Callbacks /*callbacks*/) {
  // NOLINTEND(performance-unnecessary-value-param)
  return Result<std::unique_ptr<MediaSession>>::failure(
      "media_unavailable",
      "this client was built without libwebrtc, so it cannot send or receive audio. "
      "See docs/webrtc-toolchain.md.");
}

bool media_is_available() noexcept {
  return false;
}

HardwareEncoding hardware_encoding() {
  return HardwareEncoding{
      .compiled_in = false,
      .available = false,
      .implementation = {},
      .detail = "this client was built without libwebrtc, so it encodes nothing at all",
  };
}

Result<std::vector<AudioDevice>> input_devices() {
  return Result<std::vector<AudioDevice>>::failure(
      "media_unavailable", "this client was built without libwebrtc, so it has no audio devices");
}

Result<std::vector<AudioDevice>> output_devices() {
  return input_devices();
}

#endif  // DV_WITH_LIBWEBRTC

}  // namespace dv::client::media
